#include "fileops.h"
#include "mimeapps.h"
#include "transferprogress.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <memory>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

/* ------------------------------------------------------------ ownership */

namespace {

// Filesystems that have no Unix owners (FAT, exFAT, NTFS, SMB, FUSE, ...).
// GNU cp -a run as root reports "failed to preserve ownership" there and
// exits 1 even though the data was copied; as a normal user the same failure
// is silent.  The GTK3 filer never treated it as an error (action.c only
// g_debug()s a failed lchown), so the Qt port must not either.  On ext4,
// btrfs, xfs, tmpfs... ownership is still preserved exactly as before.
bool destinationLacksOwnership(const QString &dir)
{
    struct statfs st;
    if (statfs(QFile::encodeName(dir).constData(), &st) != 0)
        return false;  // unknown: keep the old, ownership-preserving behaviour
    switch (static_cast<unsigned long>(st.f_type)) {
    case 0x4d44UL:      // MSDOS_SUPER_MAGIC (vfat/msdos)
    case 0x2011BAB0UL:  // EXFAT_SUPER_MAGIC
    case 0x5346544eUL:  // NTFS_SB_MAGIC (legacy ntfs)
    case 0x7366746eUL:  // ntfs3
    case 0x65735546UL:  // FUSE (ntfs-3g, exfat-fuse, sshfs...)
    case 0xFF534D42UL:  // CIFS_MAGIC_NUMBER
    case 0xFE534D42UL:  // SMB2_MAGIC_NUMBER
    case 0x517BUL:      // SMB_SUPER_MAGIC
    case 0x4244UL:      // HFS_SUPER_MAGIC
    case 0x15013346UL:  // UDF_SUPER_MAGIC
    case 0x786f4256UL:  // VBOXSF
        return true;
    default:
        return false;
    }
}

} // namespace

/* ------------------------------------------------------------------ FileJob */

FileJob *FileJob::run(QWidget *parent, const QString &title,
                      const QList<QStringList> &commands, Done done,
                      const QString &workingDir)
{
    auto *job = new FileJob(parent, title, commands, std::move(done), workingDir);
    QTimer::singleShot(0, job, &FileJob::next);
    return job;
}

FileJob *FileJob::runTracked(QWidget *parent, const QString &title,
                             const QList<QStringList> &commands,
                             TransferProgress *tracker, Done done)
{
    auto *job = new FileJob(parent, title, commands, std::move(done), QString());
    job->attachTracker(tracker);  // next() starts once the total is known
    return job;
}

FileJob::FileJob(QWidget *parent, const QString &title, const QList<QStringList> &commands,
                 Done done, const QString &workingDir)
    : QObject(nullptr), m_parent(parent), m_commands(commands),
      m_done(std::move(done)), m_workingDir(workingDir)
{
    m_progress = new QProgressDialog(title, QCoreApplication::translate("FileOps", "Cancel"),
                                     0, qMax(1, int(commands.size())), parent);
    m_progress->setWindowTitle(title);
    m_progress->setWindowModality(Qt::NonModal);
    m_progress->setMinimumDuration(700);
    m_progress->setMinimumWidth(600);
    if (auto *label = m_progress->findChild<QLabel *>())
        label->setWordWrap(true);
    m_progress->setAutoClose(false);
    m_progress->setAutoReset(false);
    m_progress->setValue(0);
    m_labelBase = title;
    connect(m_progress, &QProgressDialog::canceled, this, [this] {
        // Once every command has exited the data is in the kernel's hands:
        // nothing is left to cancel, keep waiting for the flush.
        if (m_flushRequested)
            return;
        m_cancelled = true;
        if (m_process && m_process->state() != QProcess::NotRunning)
            m_process->terminate();
        else if (m_tracker && !m_commandsStarted)
            finish();  // still counting the source size
    });
}

void FileJob::attachTracker(TransferProgress *tracker)
{
    m_tracker = tracker;
    connect(tracker, &QThread::finished, tracker, &QObject::deleteLater);
    connect(tracker, &TransferProgress::counted, this, [this](qint64) {
        if (m_cancelled || m_finished)
            return;
        m_progress->setRange(0, 1000);
        m_clock.start();
        next();
    });
    connect(tracker, &TransferProgress::flushed, this, &FileJob::finish);
    m_progress->setRange(0, 0);  // busy until the size is known
    m_ticker = new QTimer(this);
    m_ticker->setInterval(250);
    connect(m_ticker, &QTimer::timeout, this, &FileJob::updateBytes);
    m_ticker->start();
    updateBytes();
    tracker->start();
}

void FileJob::updateBytes()
{
    if (!m_progress || !m_tracker)
        return;
    const qint64 total = m_tracker->totalBytes();
    QString stats;
    if (total < 0) {
        stats = QCoreApplication::translate("FileOps", "Calculating size…");
    } else {
        const qint64 done = m_tracker->doneBytes();
        // 100% only once flushed() arrives; finish() sets the maximum.
        const int value = total > 0 ? int(qMin<qint64>(999, done * 1000 / total)) : 999;
        m_progress->setValue(value);
        const QLocale locale;
        stats = QCoreApplication::translate("FileOps", "%1 of %2")
                    .arg(locale.formattedDataSize(done), locale.formattedDataSize(total));
        if (done > 0 && done < total && m_clock.isValid() && m_clock.elapsed() > 1500) {
            // Same estimate as the GTK3 abox_set_percentage().
            const qint64 seconds = qint64(double(m_clock.elapsed()) / 1000.0
                                          * double(total - done) / double(done));
            const QString duration = seconds >= 3600
                ? QStringLiteral("%1:%2:%3").arg(seconds / 3600).arg((seconds % 3600) / 60, 2, 10, QLatin1Char('0'))
                      .arg(seconds % 60, 2, 10, QLatin1Char('0'))
                : QStringLiteral("%1:%2").arg(seconds / 60, 2, 10, QLatin1Char('0'))
                      .arg(seconds % 60, 2, 10, QLatin1Char('0'));
            stats += QStringLiteral("    ")
                + QCoreApplication::translate("FileOps", "Remaining: %1").arg(duration);
        }
        // cp/mv have exited: the rest is the kernel writing to the device.
        if (m_flushRequested)
            stats += QLatin1Char('\n')
                + QCoreApplication::translate("FileOps", "Writing to the device. Do not remove it until this window closes.");
    }
    m_progress->setLabelText(m_labelBase + QLatin1Char('\n') + stats);
}

void FileJob::next()
{
    m_commandsStarted = true;
    if (m_tracker && !m_cancelled && m_index >= m_commands.size()) {
        if (!m_flushRequested) {
            m_flushRequested = true;
            m_progress->setCancelButton(nullptr);
            m_tracker->commandsFinished();  // flushed() -> finish()
            updateBytes();
        }
        return;
    }
    if (m_cancelled || m_index >= m_commands.size()) {
        finish();
        return;
    }
    const QStringList cmd = m_commands.at(m_index);
    if (cmd.isEmpty()) {
        ++m_index;
        next();
        return;
    }
    if (m_progress) {
        if (!m_tracker)
            m_progress->setValue(m_index);
        // The GTK copy/move window shows complete routes. Do not collapse
        // them to QFileInfo(...).fileName() -- the user needs exact paths.
        const QString program = QFileInfo(cmd.first()).fileName();
        QString display;
        if ((program == QLatin1String("cp") || program == QLatin1String("mv") ||
             (program == QLatin1String("sh") && cmd.size() >= 6 && cmd.at(2).contains(QLatin1String("cp -a")))) && cmd.size() >= 5) {
            display = QCoreApplication::translate("FileOps", "Source: %1\nDestination: %2")
                          .arg(cmd.at(cmd.size() - 2), cmd.last());
        } else {
            display = cmd.last();
        }
        m_labelBase = m_progress->windowTitle() + QLatin1Char('\n') + display;
        m_progress->setLabelText(m_labelBase);
        m_progress->setToolTip(display);
        if (m_tracker)
            updateBytes();
    }

    m_process = new QProcess(this);
    if (!m_workingDir.isEmpty())
        m_process->setWorkingDirectory(m_workingDir);
    auto handled = std::make_shared<bool>(false);
    auto complete = [this, cmd, handled](bool ok, const QString &message) {
        if (*handled)
            return;
        *handled = true;
        if (!ok && !m_cancelled) {
            m_errors += message.isEmpty()
                ? QCoreApplication::translate("FileOps", "Command failed: %1").arg(cmd.join(QLatin1Char(' ')))
                : message;
            m_errors += QLatin1Char('\n');
        }
        m_process->deleteLater();
        m_process = nullptr;
        ++m_index;
        next();
    };
    connect(m_process, &QProcess::finished, this,
            [this, complete](int code, QProcess::ExitStatus status) {
        const QString err = QString::fromLocal8Bit(m_process->readAllStandardError()).trimmed();
        complete(status == QProcess::NormalExit && code == 0, err);
    });
    connect(m_process, &QProcess::errorOccurred, this, [complete, cmd](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            complete(false, QCoreApplication::translate("FileOps", "Could not start %1.").arg(cmd.first()));
    });
    m_process->start(cmd.first(), cmd.mid(1));
}

void FileJob::finish()
{
    if (m_finished)
        return;
    m_finished = true;
    if (m_ticker)
        m_ticker->stop();
    if (m_tracker)
        m_tracker->cancel();  // no-op after flushed(); the thread deletes itself
    if (m_progress) {
        m_progress->setValue(m_progress->maximum());
        m_progress->deleteLater();
    }
    if (m_done)
        m_done(m_errors.isEmpty() && !m_cancelled, m_errors.trimmed());
    deleteLater();
}

/* ------------------------------------------------------------------ helpers */

namespace {

void showErrors(QWidget *parent, const QString &title, const QString &errors)
{
    if (errors.isEmpty())
        return;
    QMessageBox box(QMessageBox::Warning, title,
                    QCoreApplication::translate("FileOps", "The operation finished with errors."),
                    QMessageBox::Ok, parent);
    box.setDetailedText(errors);
    box.exec();
}

QStringList localPathsFromUrls(const QList<QUrl> &urls)
{
    QStringList out;
    for (const QUrl &u : urls) {
        if (u.isLocalFile()) {
            const QString p = u.toLocalFile();
            if (QFileInfo::exists(p) || QFileInfo(p).isSymLink())
                out << p;
        }
    }
    return out;
}


} // namespace

namespace FileOps {

FileTransferEvents *events()
{
    static FileTransferEvents *shared = new FileTransferEvents(QCoreApplication::instance());
    return shared;
}

/* ---------------------------------------------------------------- clipboard */

void setClipboard(const QStringList &paths, bool cut)
{
    auto *md = new QMimeData;
    QList<QUrl> urls;
    QByteArray gnome = cut ? QByteArrayLiteral("cut") : QByteArrayLiteral("copy");
    for (const QString &p : paths) {
        const QUrl u = QUrl::fromLocalFile(p);
        urls << u;
        gnome += '\n';
        gnome += u.toEncoded();
    }
    md->setUrls(urls);
    md->setData(QStringLiteral("x-special/gnome-copied-files"), gnome);
    md->setData(QStringLiteral("application/x-kde-cutselection"), cut ? "1" : "0");
    md->setText(paths.join(QLatin1Char('\n')));
    QGuiApplication::clipboard()->setMimeData(md);
}

QStringList clipboardPaths(bool *cut)
{
    if (cut) *cut = false;
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    if (!md)
        return QStringList();

    const QString gnomeType = QStringLiteral("x-special/gnome-copied-files");
    if (md->hasFormat(gnomeType)) {
        const QList<QByteArray> lines = md->data(gnomeType).split('\n');
        QList<QUrl> urls;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray l = lines.at(i).trimmed();
            if (!l.isEmpty())
                urls << QUrl::fromEncoded(l);
        }
        if (cut && !lines.isEmpty())
            *cut = lines.first().trimmed() == "cut";
        return localPathsFromUrls(urls);
    }
    if (md->hasUrls()) {
        if (cut)
            *cut = md->data(QStringLiteral("application/x-kde-cutselection")).trimmed() == "1";
        return localPathsFromUrls(md->urls());
    }
    return QStringList();
}

bool clipboardHasFiles()
{
    return !clipboardPaths(nullptr).isEmpty();
}

// Do not accept remote URLs or partially transfer a mixed local/remote drag.
QStringList pathsFromMime(const QMimeData *mime)
{
    if (!mime || !mime->hasUrls() || mime->urls().isEmpty())
        return {};
    for (const QUrl &url : mime->urls()) {
        if (!url.isLocalFile())
            return {};
    }
    const QStringList paths = localPathsFromUrls(mime->urls());
    return paths.size() == mime->urls().size() ? paths : QStringList{};
}

/* ----------------------------------------------------------------- naming */

QString uniqueName(const QString &dir, const QString &leaf)
{
    const QDir d(dir);
    if (!QFileInfo::exists(d.filePath(leaf)) && !QFileInfo(d.filePath(leaf)).isSymLink())
        return leaf;
    QString base = leaf;
    QString ext;
    const int dot = leaf.lastIndexOf(QLatin1Char('.'));
    if (dot > 0 && !QFileInfo(d.filePath(leaf)).isDir()) {
        base = leaf.left(dot);
        ext = leaf.mid(dot);
    }
    for (int n = 1; n < 10000; ++n) {
        const QString candidate = n == 1
            ? QStringLiteral("%1 (%2)%3").arg(base, QCoreApplication::translate("FileOps", "copy"), ext)
            : QStringLiteral("%1 (%2 %3)%4").arg(base, QCoreApplication::translate("FileOps", "copy")).arg(n).arg(ext);
        if (!QFileInfo::exists(d.filePath(candidate)) && !QFileInfo(d.filePath(candidate)).isSymLink())
            return candidate;
    }
    return leaf + QStringLiteral(".new");
}

bool isInside(const QString &path, const QString &dir)
{
    const QString p = QDir::cleanPath(path);
    const QString d = QDir::cleanPath(dir);
    return p == d || p.startsWith(d.endsWith(QLatin1Char('/')) ? d : d + QLatin1Char('/'));
}

/* ------------------------------------------------ full destination path */

QString resolveSingleDestination(const QString &source, const QString &typed, bool move)
{
    if (!QDir::isAbsolutePath(typed))
        return QString();
    const QString clean = QDir::cleanPath(typed);
    QString dest = clean;
    const QFileInfo info(clean);
    if (typed.endsWith(QLatin1Char('/')) || info.isDir()) {
        if (!info.isDir()) return QString(); // never create missing directories implicitly
        dest = QDir(clean).filePath(QFileInfo(source).fileName());
    }
    // Absolute path equal to original? A COPY means an adjacent duplicate;
    // MOVE is a no-op and is rejected by the caller.
    if (!move && QDir::cleanPath(dest) == QDir::cleanPath(source)) {
        const QFileInfo src(source);
        dest = QDir(src.absolutePath()).filePath(uniqueName(src.absolutePath(), src.fileName()));
    }
    return dest;
}

/* ------------------------------------------------------------ copy / move */

void transfer(QWidget *parent, const QStringList &sources, const QString &destDir,
              bool move, std::function<void(bool)> done, const QString &destinationName)
{
    // Never run a file operation with an implicit/fallback source. The caller
    // must provide an explicit, absolute, non-empty selection.
    if (sources.isEmpty() || !QDir::isAbsolutePath(destDir)) {
        if (done) done(false);
        return;
    }
    for (const QString &source : sources) {
        if (!QDir::isAbsolutePath(source) || source.isEmpty() ||
            QDir::cleanPath(source) == QLatin1String("/")) {
            QMessageBox::warning(parent, QCoreApplication::translate("FileOps", "File operation"),
                                 QCoreApplication::translate("FileOps", "Invalid source path."));
            if (done) done(false);
            return;
        }
    }
    if (!QFileInfo(destDir).isDir()) {
        QMessageBox::warning(parent, QCoreApplication::translate("FileOps", "File operation"),
            QCoreApplication::translate("FileOps", "The destination folder is not available."));
        if (done) done(false);
        return;
    }
    if (!destinationName.isEmpty() && (sources.size() != 1 ||
        destinationName == QLatin1String(".") || destinationName == QLatin1String("..") ||
        destinationName.contains(QLatin1Char('/')))) {
        QMessageBox::warning(parent, QCoreApplication::translate("FileOps", "File operation"),
                             QCoreApplication::translate("FileOps", "Invalid destination filename."));
        if (done) done(false);
        return;
    }
    struct Pair { QString src; QString dst; bool conflict = false; bool dirMerge = false; };
    QList<Pair> pairs;
    QStringList refused;

    for (const QString &src : sources) {
        const QFileInfo sfi(src);
        if (!sfi.exists() && !sfi.isSymLink()) {
            refused << sfi.fileName();
            continue;
        }
        // Canonical paths protect against copying a directory into itself
        // through a symlinked destination (an otherwise unbounded recursion).
        if (sfi.isDir() && !sfi.isSymLink() &&
            isInside(QFileInfo(destDir).canonicalFilePath(), sfi.canonicalFilePath())) {
            refused << sfi.fileName();
            continue;
        }
        Pair p;
        p.src = src;
        p.dst = QDir(destDir).filePath(destinationName.isEmpty() ? sfi.fileName() : destinationName);
        if (QDir::cleanPath(p.dst) == QDir::cleanPath(sfi.absoluteFilePath()) &&
            !destinationName.isEmpty()) {
            refused << sfi.fileName(); // never overwrite the source with itself
            continue;
        }
        if (destinationName.isEmpty() && QDir::cleanPath(sfi.absolutePath()) == QDir::cleanPath(destDir)) {
            if (move)
                continue; // moving onto itself: nothing to do
            p.dst = QDir(destDir).filePath(uniqueName(destDir, sfi.fileName()));
        } else if (QFileInfo::exists(p.dst) || QFileInfo(p.dst).isSymLink()) {
            p.conflict = true;
        }
        pairs << p;
    }

    if (!refused.isEmpty())
        QMessageBox::warning(parent, move ? QCoreApplication::translate("FileOps", "Move") : QCoreApplication::translate("FileOps", "Copy"),
                             QCoreApplication::translate("FileOps", "A folder cannot be copied or moved into itself:\n%1")
                                 .arg(refused.join(QLatin1Char('\n'))));

    // Fail-safe: directories are recursive by nature (cp -a). Show their FULL
    // source paths and require an explicit confirmation before starting.
    // The default action is Cancel, never Copy/Move. This also catches an
    // accidentally selected /home/user in place of three intended files.
    QStringList recursiveSources;
    for (const Pair &pair : std::as_const(pairs)) {
        const QFileInfo fi(pair.src);
        if (fi.isDir() && !fi.isSymLink())
            recursiveSources << fi.absoluteFilePath();
    }
    if (!recursiveSources.isEmpty() && parent) {
        QMessageBox warning(QMessageBox::Warning,
                            move ? QCoreApplication::translate("FileOps", "Move")
                                 : QCoreApplication::translate("FileOps", "Copy"),
                            QCoreApplication::translate("FileOps",
                                "The selection includes folders. Their entire contents will be processed. Continue?"),
                            QMessageBox::NoButton, parent);
        warning.setInformativeText(recursiveSources.mid(0, 5).join(QLatin1Char('\n')));
        warning.setDetailedText(QCoreApplication::translate("FileOps", "Selected items:") +
                                QLatin1Char('\n') + sources.join(QLatin1Char('\n')) +
                                QLatin1Char('\n') +
                                QCoreApplication::translate("FileOps", "Destination folder:") +
                                QLatin1Char('\n') + destDir);
        QPushButton *cancel = warning.addButton(QMessageBox::Cancel);
        QPushButton *proceed = warning.addButton(move ? QCoreApplication::translate("FileOps", "Move")
                                                      : QCoreApplication::translate("FileOps", "Copy"),
                                                    QMessageBox::AcceptRole);
        warning.setDefaultButton(cancel);
        warning.exec();
        if (warning.clickedButton() != proceed) {
            if (done) done(false);
            return;
        }
    }

    int conflicts = 0;
    for (const Pair &p : std::as_const(pairs))
        if (p.conflict) ++conflicts;

    enum { Replace, Skip, KeepBoth } policy = Replace;
    if (conflicts > 0) {
        QMessageBox box(QMessageBox::Question, move ? QCoreApplication::translate("FileOps", "Move") : QCoreApplication::translate("FileOps", "Copy"),
                        conflicts == 1
                            ? QCoreApplication::translate("FileOps", "An item with the same name already exists in the destination.")
                            : QCoreApplication::translate("FileOps", "%1 items already exist in the destination.").arg(conflicts),
                        QMessageBox::NoButton, parent);
        QPushButton *replace = box.addButton(QCoreApplication::translate("FileOps", "Replace"), QMessageBox::DestructiveRole);
        QPushButton *skip = box.addButton(QCoreApplication::translate("FileOps", "Skip"), QMessageBox::ActionRole);
        QPushButton *both = box.addButton(QCoreApplication::translate("FileOps", "Keep Both"), QMessageBox::ActionRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(both);
        box.exec();
        if (box.clickedButton() == replace) policy = Replace;
        else if (box.clickedButton() == skip) policy = Skip;
        else if (box.clickedButton() == both) policy = KeepBoth;
        else return;
    }

    QList<QStringList> commands;
    QList<TransferProgress::Item> tracked;
    QStringList completedDestinations;
    bool skippedItems = !refused.isEmpty();
    struct stat destStat {};
    const bool destStatOk = ::stat(QFile::encodeName(destDir).constData(), &destStat) == 0;
    // cp -a, minus ownership when the destination cannot store owners.
    QStringList cpArgs{QStringLiteral("-a")};
    QString cpScriptArgs = QStringLiteral("-a");
    if (destinationLacksOwnership(destDir)) {
        cpArgs << QStringLiteral("--no-preserve=ownership");
        cpScriptArgs += QStringLiteral(" --no-preserve=ownership");
    }
    for (Pair p : std::as_const(pairs)) {
        if (p.conflict) {
            const bool srcDir = QFileInfo(p.src).isDir() && !QFileInfo(p.src).isSymLink();
            const bool dstDir = QFileInfo(p.dst).isDir() && !QFileInfo(p.dst).isSymLink();
            if (policy == Skip) {
                skippedItems = true;
                continue;
            }
            if (policy == KeepBoth || srcDir != dstDir)
                p.dst = QDir(destDir).filePath(uniqueName(destDir, QFileInfo(p.src).fileName()));
            else if (srcDir && dstDir)
                p.dirMerge = true;
        }
        completedDestinations << p.dst;
        // Bytes are written for copies, folder merges (cp + rm) and moves to
        // another filesystem; a move on the same filesystem is a rename.
        struct stat srcStat {};
        const bool sameDevice = destStatOk
            && ::lstat(QFile::encodeName(p.src).constData(), &srcStat) == 0
            && srcStat.st_dev == destStat.st_dev;
        if (!move || p.dirMerge || !sameDevice)
            tracked << TransferProgress::Item{p.src, p.dst};
        if (p.dirMerge) {
            // Merge directory contents; for a move delete the source only if
            // the copy succeeded.
            const QString script = move
                ? QStringLiteral("cp %1 -- \"$1\"/. \"$2\"/ && rm -rf -- \"$1\"").arg(cpScriptArgs)
                : QStringLiteral("cp %1 -- \"$1\"/. \"$2\"/").arg(cpScriptArgs);
            commands << QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-c"), script,
                                    QStringLiteral("sh"), p.src, p.dst};
        } else if (move) {
            commands << QStringList{QStringLiteral("mv"), QStringLiteral("-f"), QStringLiteral("--"), p.src, p.dst};
        } else {
            commands << (QStringList{QStringLiteral("cp")} + cpArgs
                         + QStringList{QStringLiteral("--"), p.src, p.dst});
        }
    }
    if (commands.isEmpty()) {
        if (done) done(false);
        return;
    }

    const QString title = move ? QCoreApplication::translate("FileOps", "Moving…") : QCoreApplication::translate("FileOps", "Copying…");
    QPointer<QWidget> guard(parent);
    auto finished = [guard, move, skippedItems, completedDestinations, done](bool ok, const QString &errors) {
        showErrors(guard, move ? QCoreApplication::translate("FileOps", "Move") : QCoreApplication::translate("FileOps", "Copy"), errors);
        if (ok) emit events()->destinationsCreated(completedDestinations);
        if (done) done(ok && !skippedItems);
    };
    if (tracked.isEmpty())
        FileJob::run(parent, title, commands, finished);
    else
        FileJob::runTracked(parent, title, commands, new TransferProgress(tracked), finished);
}

void deletePermanently(QWidget *parent, const QStringList &paths, std::function<void()> done)
{
    if (paths.isEmpty())
        return;
    const QString question = paths.size() == 1
        ? QCoreApplication::translate("FileOps", "Permanently delete “%1”? This cannot be undone.").arg(QFileInfo(paths.first()).fileName())
        : QCoreApplication::translate("FileOps", "Permanently delete %1 items? This cannot be undone.").arg(paths.size());
    if (QMessageBox::warning(parent, QCoreApplication::translate("FileOps", "Delete Permanently"), question,
                             QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel) != QMessageBox::Yes)
        return;

    QList<QStringList> commands;
    for (const QString &p : paths)
        commands << QStringList{QStringLiteral("rm"), QStringLiteral("-rf"), QStringLiteral("--"), p};
    QPointer<QWidget> guard(parent);
    FileJob::run(parent, QCoreApplication::translate("FileOps", "Deleting…"), commands, [guard, done](bool, const QString &errors) {
        showErrors(guard, QCoreApplication::translate("FileOps", "Delete"), errors);
        if (done) done();
    });
}

QStringList moveToTrash(const QStringList &paths)
{
    QStringList failed;
    for (const QString &p : paths) {
        if (!QFile::moveToTrash(p))
            failed << QFileInfo(p).fileName();
    }
    return failed;
}

/* ------------------------------------------------------------------- trash */

bool isTrashFilesDir(const QString &dir)
{
    const QFileInfo fi(QDir::cleanPath(dir));
    if (fi.fileName() != QLatin1String("files"))
        return false;
    const QDir root(fi.absolutePath());
    if (!QFileInfo(root.filePath(QStringLiteral("info"))).isDir())
        return false;
    const QString rootName = QFileInfo(root.absolutePath()).fileName();
    const QString parentName = QFileInfo(QFileInfo(root.absolutePath()).absolutePath()).fileName();
    return rootName == QLatin1String("Trash") || rootName.startsWith(QLatin1String(".Trash")) ||
           parentName == QLatin1String(".Trash");
}

QString trashOriginalPath(const QString &trashedPath)
{
    const QFileInfo fi(trashedPath);
    const QString filesDir = fi.absolutePath();
    const QString trashRoot = QFileInfo(filesDir).absolutePath();
    const QString infoPath = trashRoot + QStringLiteral("/info/") + fi.fileName() + QStringLiteral(".trashinfo");
    QFile f(infoPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (!line.startsWith("Path="))
            continue;
        QString original = QUrl::fromPercentEncoding(line.mid(5));
        if (QDir::isAbsolutePath(original))
            return QDir::cleanPath(original);
        // Top-directory trash: $topdir/.Trash-$uid or $topdir/.Trash/$uid
        const QString rootName = QFileInfo(trashRoot).fileName();
        QString topdir = QFileInfo(trashRoot).absolutePath();
        if (!rootName.startsWith(QLatin1String(".Trash")))
            topdir = QFileInfo(topdir).absolutePath();
        return QDir::cleanPath(topdir + QLatin1Char('/') + original);
    }
    return QString();
}

void restoreFromTrash(QWidget *parent, const QStringList &paths, std::function<void()> done)
{
    QList<QStringList> commands;
    QStringList problems;
    for (const QString &p : paths) {
        const QString original = trashOriginalPath(p);
        if (original.isEmpty()) {
            problems << QCoreApplication::translate("FileOps", "%1: no restore information").arg(QFileInfo(p).fileName());
            continue;
        }
        if (QFileInfo::exists(original)) {
            problems << QCoreApplication::translate("FileOps", "%1: “%2” already exists").arg(QFileInfo(p).fileName(), original);
            continue;
        }
        const QFileInfo fi(p);
        const QString info = QFileInfo(fi.absolutePath()).absolutePath() + QStringLiteral("/info/")
                             + fi.fileName() + QStringLiteral(".trashinfo");
        commands << QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                QStringLiteral("mkdir -p -- \"$(dirname -- \"$2\")\" && mv -- \"$1\" \"$2\" && rm -f -- \"$3\""),
                                QStringLiteral("sh"), p, original, info};
    }
    if (!problems.isEmpty())
        showErrors(parent, QCoreApplication::translate("FileOps", "Restore"), problems.join(QLatin1Char('\n')));
    if (commands.isEmpty())
        return;
    QPointer<QWidget> guard(parent);
    FileJob::run(parent, QCoreApplication::translate("FileOps", "Restoring…"), commands, [guard, done](bool, const QString &errors) {
        showErrors(guard, QCoreApplication::translate("FileOps", "Restore"), errors);
        if (done) done();
    });
}

/* ------------------------------------------------------- link / duplicate */

bool createLink(QWidget *parent, const QString &target, bool relative)
{
    const QFileInfo tfi(target);
    const QString dir = tfi.absolutePath();
    bool ok = false;
    const QString title = relative ? QCoreApplication::translate("FileOps", "Link (relative)") : QCoreApplication::translate("FileOps", "Link (absolute)");
    const QString name = QInputDialog::getText(parent, title,
        QCoreApplication::translate("FileOps", "Name or path of the new link:"), QLineEdit::Normal,
        uniqueName(dir, tfi.fileName()), &ok).trimmed();
    if (!ok || name.isEmpty())
        return false;
    const QString linkPath = QDir::isAbsolutePath(name) ? QDir::cleanPath(name)
                                                        : QDir::cleanPath(QDir(dir).filePath(name));
    if (QFileInfo::exists(linkPath) || QFileInfo(linkPath).isSymLink()) {
        QMessageBox::warning(parent, title, QCoreApplication::translate("FileOps", "“%1” already exists.").arg(linkPath));
        return false;
    }
    const QString absTarget = tfi.absoluteFilePath();
    const QString value = relative
        ? QDir(QFileInfo(linkPath).absolutePath()).relativeFilePath(absTarget)
        : absTarget;
    if (::symlink(QFile::encodeName(value).constData(), QFile::encodeName(linkPath).constData()) != 0) {
        QMessageBox::warning(parent, title, QCoreApplication::translate("FileOps", "Could not create the link: %1")
                                                .arg(QString::fromLocal8Bit(strerror(errno))));
        return false;
    }
    return true;
}

// Create links in any destination, preserving multi-selection and the exact
// spelling of symbolic-link targets (no QFileInfo canonicalisation).
bool createLinksIn(QWidget *parent, const QStringList &sources,
                   const QString &destDir, bool relative)
{
    if (!QFileInfo(destDir).isDir() || sources.isEmpty())
        return false;
    QStringList errors;
    QStringList createdLinks;
    int created = 0;
    for (const QString &source : sources) {
        QFileInfo info(source);
        if (!info.exists() && !info.isSymLink()) {
            errors << QCoreApplication::translate("FileOps", "Source does not exist: %1").arg(source);
            continue;
        }
        const QString absSource = info.absoluteFilePath();
        const QString name = uniqueName(destDir, info.fileName());
        const QString linkPath = QDir(destDir).filePath(name);
        const QString value = relative
            ? QDir(QFileInfo(linkPath).absolutePath()).relativeFilePath(absSource)
            : absSource;
        const QByteArray encodedTarget = QFile::encodeName(value);
        const QByteArray encodedLink = QFile::encodeName(linkPath);
        if (::symlink(encodedTarget.constData(), encodedLink.constData()) != 0)
            errors << QStringLiteral("%1: %2").arg(name, QString::fromLocal8Bit(strerror(errno)));
        else {
            ++created;
            createdLinks << linkPath;
        }
    }
    if (!createdLinks.isEmpty())
        emit events()->destinationsCreated(createdLinks); // highlight the new links too
    if (!errors.isEmpty()) {
        QMessageBox::warning(parent, QCoreApplication::translate("FileOps", "Link"),
                             QCoreApplication::translate("FileOps", "Could not create some links:\n%1")
                                  .arg(errors.join(QLatin1Char('\n'))));
    }
    return created > 0 && errors.isEmpty();
}

bool duplicate(QWidget *parent, const QString &path, std::function<void()> done)
{
    // Compatibility entry point. The normal Ctrl+D action uses FilePane's
    // full-width dialog, with the complete absolute destination editable.
    const QFileInfo fi(path);
    const QString dir = fi.absolutePath();
    const QString initial = QDir(dir).filePath(uniqueName(dir, fi.fileName()));
    bool ok = false;
    const QString typed = QInputDialog::getText(
        parent, QCoreApplication::translate("FileOps", "Duplicate"),
        QCoreApplication::translate("FileOps", "Full destination path:"),
        QLineEdit::Normal, initial, &ok);
    if (!ok || typed.isEmpty()) return false;
    if (!QDir::isAbsolutePath(typed)) {
        QMessageBox::warning(parent, QCoreApplication::translate("FileOps", "Duplicate"),
                             QCoreApplication::translate("FileOps", "The destination must be an absolute path."));
        return false;
    }
    const QString target = resolveSingleDestination(path, typed, false);
    if (target.isEmpty() || !QFileInfo(target).absoluteDir().exists()) {
        QMessageBox::warning(parent, QCoreApplication::translate("FileOps", "Duplicate"),
                             QCoreApplication::translate("FileOps", "The destination folder is not available."));
        return false;
    }
    transfer(parent, {path}, QFileInfo(target).absolutePath(), false,
             [done](bool) { if (done) done(); }, QFileInfo(target).fileName());
    return true;
}

/* ---------------------------------------------------------------- terminal */

QStringList runInTerminalArgs(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.isFile())
        return QStringList();
    const QString lower = fi.fileName().toLower();
    if (lower.endsWith(QLatin1String(".desktop")))
        return QStringList();

    QStringList argv;
    if (lower.endsWith(QLatin1String(".appimage"))) {
        if (!fi.isExecutable())
            return QStringList();
        argv << fi.absoluteFilePath();
    } else {
        QFile f(path);
        QByteArray first;
        if (f.open(QIODevice::ReadOnly))
            first = f.readLine(512).trimmed();
        if (first.startsWith("#!")) {
            if (fi.isExecutable()) {
                argv << fi.absoluteFilePath();
            } else {
                // Parse the interpreter ourselves (handles CRLF and env -S).
                QStringList interp = QString::fromLocal8Bit(first.mid(2)).trimmed()
                                         .split(QLatin1Char(' '), Qt::SkipEmptyParts);
                if (interp.isEmpty())
                    return QStringList();
                argv << interp << fi.absoluteFilePath();
            }
        } else {
            static const struct { const char *ext; const char *interp; } map[] = {
                {".bash", "bash"}, {".ash", "ash"}, {".dash", "dash"}, {".zsh", "zsh"},
                {".ksh", "ksh"}, {".fish", "fish"}, {".sh", "sh"}, {".py", "python3"},
                {".pyw", "python3"}, {".pl", "perl"}, {".rb", "ruby"}, {".lua", "lua"},
            };
            for (const auto &m : map) {
                if (lower.endsWith(QLatin1String(m.ext))) {
                    argv << QString::fromLatin1(m.interp) << fi.absoluteFilePath();
                    break;
                }
            }
            if (argv.isEmpty() && fi.isExecutable()) {
                static const QMimeDatabase db;
                const QString mime = db.mimeTypeForFile(fi).name();
                if (mime == QLatin1String("application/x-executable") ||
                    mime == QLatin1String("application/x-pie-executable") ||
                    mime == QLatin1String("application/x-sharedlib") ||
                    mime == QLatin1String("application/x-shellscript"))
                    argv << fi.absoluteFilePath();
            }
        }
    }
    if (argv.isEmpty())
        return QStringList();

    // Keep the terminal open after the program ends, like the GTK build.
    const QString hold = QStringLiteral(
        "prompt=\"$1\"; shift; \"$@\"; status=$?; echo; "
        "printf '[%s] %s' \"$status\" \"$prompt\"; read _");
    QStringList inner{QStringLiteral("/bin/sh"), QStringLiteral("-c"), hold, QStringLiteral("sh"),
                      QCoreApplication::translate("FileOps", "Press Enter to close.")};
    inner << argv;
    return MimeApps::wrapInTerminal(inner);
}

bool openTerminalIn(const QString &dir)
{
    const QString term = MimeApps::findTerminal();
    if (term.isEmpty())
        return false;
    return QProcess::startDetached(term, {}, dir);
}

/* --------------------------------------------------------------- templates */

QString userTemplatesDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/rox.sourceforge.net/Templates");
}

QStringList templateFiles()
{
    QStringList dirs{userTemplatesDir()};
    const QString xdg = QStandardPaths::writableLocation(QStandardPaths::TemplatesLocation);
    if (!xdg.isEmpty() && QDir::cleanPath(xdg) != QDir::cleanPath(QDir::homePath()))
        dirs << xdg;
    dirs << QStringLiteral(":/rox-qt/templates");

    QStringList out;
    QSet<QString> seen;
    for (const QString &d : std::as_const(dirs)) {
        const QDir dir(d);
        const QStringList entries = dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                                  QDir::Name | QDir::IgnoreCase);
        for (const QString &e : entries) {
            if (seen.contains(e))
                continue;
            seen.insert(e);
            out << dir.filePath(e);
        }
    }
    return out;
}

/* ------------------------------------------------------------- backgrounds */

namespace {
const QString kBackgrounds = QStringLiteral("/usr/share/backgrounds");
}

bool canCopyToBackgrounds(const QString &path)
{
    static const QMimeDatabase db;
    const QFileInfo fi(path);
    if (!fi.isFile() || !db.mimeTypeForFile(fi).name().startsWith(QLatin1String("image/")))
        return false;
    const QFileInfo bg(kBackgrounds);
    if (bg.exists())
        return bg.isDir() && bg.isWritable();
    return QFileInfo(bg.absolutePath()).isWritable();
}

void copyToBackgrounds(QWidget *parent, const QString &path)
{
    const QString title = QCoreApplication::translate("FileOps", "Copy to Backgrounds");
    if (!QDir().mkpath(kBackgrounds)) {
        QMessageBox::warning(parent, title, QCoreApplication::translate("FileOps", "Unable to create backgrounds folder “%1”.").arg(kBackgrounds));
        return;
    }
    const QFileInfo fi(path);
    const QString dest = kBackgrounds + QLatin1Char('/') + fi.fileName();
    if (QFileInfo(dest).canonicalFilePath() == fi.canonicalFilePath())
        return;
    if (QFileInfo::exists(dest)) {
        if (QMessageBox::question(parent, title,
                QCoreApplication::translate("FileOps", "A file named “%1” already exists in the backgrounds folder.\nReplace the existing file?")
                    .arg(fi.fileName()),
                QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        QFile::remove(dest);
    }
    if (!QFile::copy(path, dest))
        QMessageBox::warning(parent, title, QCoreApplication::translate("FileOps", "Unable to copy “%1” to “%2”.").arg(path, dest));
    else
        QFile::setPermissions(dest, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
}

/* ------------------------------------------------------------------- misc */

QString humanSize(qint64 size)
{
    return QLocale().formattedDataSize(size);
}

QString permissionString(const QString &path)
{
    struct stat st;
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0)
        return QString();
    QString s(10, QLatin1Char('-'));
    if (S_ISDIR(st.st_mode)) s[0] = QLatin1Char('d');
    else if (S_ISLNK(st.st_mode)) s[0] = QLatin1Char('l');
    else if (S_ISCHR(st.st_mode)) s[0] = QLatin1Char('c');
    else if (S_ISBLK(st.st_mode)) s[0] = QLatin1Char('b');
    else if (S_ISFIFO(st.st_mode)) s[0] = QLatin1Char('p');
    else if (S_ISSOCK(st.st_mode)) s[0] = QLatin1Char('s');
    const mode_t bits[9] = {S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP, S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH};
    const char chars[3] = {'r', 'w', 'x'};
    for (int i = 0; i < 9; ++i)
        if (st.st_mode & bits[i]) s[i + 1] = QLatin1Char(chars[i % 3]);
    if (st.st_mode & S_ISUID) s[3] = (st.st_mode & S_IXUSR) ? QLatin1Char('s') : QLatin1Char('S');
    if (st.st_mode & S_ISGID) s[6] = (st.st_mode & S_IXGRP) ? QLatin1Char('s') : QLatin1Char('S');
    if (st.st_mode & S_ISVTX) s[9] = (st.st_mode & S_IXOTH) ? QLatin1Char('t') : QLatin1Char('T');
    return s + QStringLiteral("  (%1)").arg(st.st_mode & 07777, 4, 8, QLatin1Char('0'));
}

} // namespace FileOps

/* ImageMounter: moved to imagemounter.cpp (port of GTK3 image_mounter.c). */

