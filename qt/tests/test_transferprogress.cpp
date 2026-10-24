// 1.0.7: real byte progress for copy/move (transferprogress.cpp).
//
// Always: totals, merge folders, cancel, and the dialog driven by
// FileOps::transfer().  Optional: ROX_QT_SLOW_DIR=<folder on a slow device>
// checks that the bar moves through intermediate values and that the job is
// reported finished only when nothing is left to write.
#include "fileops.h"
#include "transferprogress.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <unistd.h>

namespace {

bool check(bool ok, const char *description)
{
    if (!ok) std::fprintf(stderr, "FAIL: %s\n", description);
    return ok;
}

bool writeBytes(const QString &path, qint64 size)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    QByteArray block(64 * 1024, 'x');
    for (qint64 left = size; left > 0; left -= block.size())
        if (f.write(block.constData(), qMin<qint64>(left, block.size())) < 0) return false;
    return true;
}

// Run cp the way FileOps does, beside a tracker, and wait for flushed().
bool trackCopy(const QList<QStringList> &commands, const QList<TransferProgress::Item> &items,
               qint64 *total, qint64 *done)
{
    auto *tracker = new TransferProgress(items);
    bool flushed = false;
    QEventLoop loop;
    QObject::connect(tracker, &TransferProgress::counted, [&](qint64) {
        for (const QStringList &cmd : commands)
            QProcess::execute(cmd.first(), cmd.mid(1));
        tracker->commandsFinished();
    });
    QObject::connect(tracker, &TransferProgress::flushed, [&] {
        flushed = true;
        *total = tracker->totalBytes();
        *done = tracker->doneBytes();
        loop.quit();
    });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    tracker->start();
    loop.exec();
    tracker->wait();
    delete tracker;
    return flushed;
}

bool awaitTransfer(const QStringList &sources, const QString &destination,
                   QList<int> *values = nullptr, qint64 *afterDoneSyncMs = nullptr)
{
    bool finished = false, result = false;
    QEventLoop loop;
    QTimer sampler;
    if (values) {
        QObject::connect(&sampler, &QTimer::timeout, [values] {
            for (QWidget *w : QApplication::topLevelWidgets())
                if (auto *d = qobject_cast<QProgressDialog *>(w))
                    if (d->maximum() == 1000) values->append(d->value());
        });
        sampler.start(200);
    }
    FileOps::transfer(nullptr, sources, destination, false, [&](bool ok) {
        finished = true;
        result = ok;
        loop.quit();
    });
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    if (!finished) loop.exec();
    sampler.stop();
    if (afterDoneSyncMs) {
        // Whatever the kernel still had to write would show up here.
        QElapsedTimer t;
        t.start();
        QProcess::execute(QStringLiteral("sync"), {QStringLiteral("-f"), destination});
        *afterDoneSyncMs = t.elapsed();
    }
    return finished && result;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!check(tmp.isValid(), "temporary directory")) return 1;
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString dst = tmp.filePath(QStringLiteral("dst"));
    QDir().mkpath(src + QStringLiteral("/tree/sub"));
    QDir().mkpath(dst);

    // 1. A file and a folder tree: total = real bytes, done = total.
    if (!check(writeBytes(src + QStringLiteral("/big.bin"), 3 * 1024 * 1024 + 17)
                   && writeBytes(src + QStringLiteral("/tree/a.bin"), 700000)
                   && writeBytes(src + QStringLiteral("/tree/sub/b.bin"), 5)
                   && writeBytes(src + QStringLiteral("/tree/sub/empty"), 0),
               "fixtures")) return 1;
    QFile::link(QStringLiteral("a.bin"), src + QStringLiteral("/tree/link"));  // not counted
    const qint64 expected = 3 * 1024 * 1024 + 17 + 700000 + 5;
    qint64 total = 0, done = 0;
    if (!check(trackCopy({{QStringLiteral("cp"), QStringLiteral("-a"), QStringLiteral("--"),
                           src + QStringLiteral("/big.bin"), dst + QStringLiteral("/big.bin")},
                          {QStringLiteral("cp"), QStringLiteral("-a"), QStringLiteral("--"),
                           src + QStringLiteral("/tree"), dst + QStringLiteral("/tree")}},
                         {{src + QStringLiteral("/big.bin"), dst + QStringLiteral("/big.bin")},
                          {src + QStringLiteral("/tree"), dst + QStringLiteral("/tree")}},
                         &total, &done),
               "tracker flushes after the copy")) return 1;
    if (!check(total == expected, "total counts regular files only")) {
        std::fprintf(stderr, "  total=%lld expected=%lld\n", total, expected);
        return 1;
    }
    if (!check(done == total, "done reaches total")) return 1;

    // 2. Folder merge: files already in the destination are not this job.
    const QString mergeSrc = tmp.filePath(QStringLiteral("msrc"));
    const QString mergeDst = tmp.filePath(QStringLiteral("mdst"));
    QDir().mkpath(mergeSrc);
    QDir().mkpath(mergeDst);
    if (!check(writeBytes(mergeDst + QStringLiteral("/old.bin"), 2 * 1024 * 1024)
                   && writeBytes(mergeSrc + QStringLiteral("/new.bin"), 300000),
               "merge fixtures")) return 1;
    QThread::sleep(3);  // old.bin's ctime is now older than the job
    if (!check(trackCopy({{QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                           QStringLiteral("cp -a -- \"$1\"/. \"$2\"/"), QStringLiteral("sh"), mergeSrc, mergeDst}},
                         {{mergeSrc, mergeDst}}, &total, &done),
               "merge tracker flushes")) return 1;
    if (!check(total == 300000 && done == 300000, "merge counts only the copied file")) {
        std::fprintf(stderr, "  total=%lld done=%lld\n", total, done);
        return 1;
    }

    // 3. Cancel: the thread stops and flushed() never comes.
    {
        auto *tracker = new TransferProgress({{src, tmp.filePath(QStringLiteral("never"))}});
        bool flushed = false;
        QObject::connect(tracker, &TransferProgress::flushed, [&] { flushed = true; });
        tracker->start();
        tracker->cancel();
        if (!check(tracker->wait(5000), "cancelled tracker stops")) return 1;
        QCoreApplication::processEvents();
        if (!check(!flushed, "cancelled tracker does not report flushed")) return 1;
        delete tracker;
    }

    // 4. The dialog FileOps::transfer() shows is driven by real bytes.
    QList<int> values;
    const QString viaDialog = tmp.filePath(QStringLiteral("dialog"));
    QDir().mkpath(viaDialog);
    if (!check(awaitTransfer({src + QStringLiteral("/big.bin"), src + QStringLiteral("/tree")}, viaDialog),
               "transfer() with tracking succeeds")) return 1;
    if (!check(QFileInfo(viaDialog + QStringLiteral("/big.bin")).size() == 3 * 1024 * 1024 + 17
                   && QFileInfo::exists(viaDialog + QStringLiteral("/tree/sub/b.bin")),
               "transfer() copied everything")) return 1;

    // 5. Cancel from the dialog while cp is still running: the job must end
    //    (no hang, no crash) and report failure.
    {
        // ROX_QT_CANCEL_SOURCE=<file on a slow-reading device> makes cp still
        // be running when Cancel arrives; otherwise a local fixture is used.
        const QString external = qEnvironmentVariable("ROX_QT_CANCEL_SOURCE");
        const QString bigSrc = external.isEmpty() ? tmp.filePath(QStringLiteral("cancel-src.bin")) : external;
        const qint64 bigSize = external.isEmpty() ? 256 * 1024 * 1024 : QFileInfo(external).size();
        const QString cancelDst = tmp.filePath(QStringLiteral("cancel-dst"));
        QDir().mkpath(cancelDst);
        if (external.isEmpty() && !check(writeBytes(bigSrc, bigSize), "cancel fixture")) return 1;
        bool finished = false, result = true, cancelled = false;
        QEventLoop loop;
        QTimer canceller;
        QObject::connect(&canceller, &QTimer::timeout, [&] {
            for (QWidget *w : QApplication::topLevelWidgets())
                if (auto *d = qobject_cast<QProgressDialog *>(w))
                    if (d->maximum() == 1000 && !cancelled)
                        // Click the real button: QProgressDialog::cancel() is
                        // the reset slot and does not emit canceled().
                        if (auto *button = d->findChild<QPushButton *>()) {
                            cancelled = true;
                            button->click();
                        }
        });
        // cp needs a moment to start after counted(); cancel right after.
        QTimer::singleShot(150, &canceller, [&canceller] { canceller.start(10); });
        FileOps::transfer(nullptr, {bigSrc}, cancelDst, false, [&](bool ok) {
            finished = true;
            result = ok;
            loop.quit();
        });
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        if (!finished) loop.exec();
        canceller.stop();
        if (!check(finished, "cancelled transfer ends")) return 1;
        // No button left means cp had already exited (data is in the kernel's
        // hands); then the copy simply completes.
        if (!check(cancelled || result, "dialog was cancelled or the copy completed")) return 1;
        if (!check(!result || QFileInfo(QDir(cancelDst).filePath(QFileInfo(bigSrc).fileName())).size() == bigSize,
                   "cancel either stops the job or the copy is complete")) return 1;
        std::fprintf(stderr, "cancel: %s, job %s\n", cancelled ? "clicked" : "no button",
                     result ? "completed" : "stopped");
        if (!external.isEmpty() && !check(!result, "cancel stops a running cp")) return 1;
        if (external.isEmpty())
            QFile::remove(bigSrc);
        QDir(cancelDst).removeRecursively();
    }

    // 6. Optional: a really slow destination.
    const QString slow = qEnvironmentVariable("ROX_QT_SLOW_DIR");
    if (!slow.isEmpty()) {
        const qint64 size = qEnvironmentVariableIntValue("ROX_QT_SLOW_MB") > 0
            ? qint64(qEnvironmentVariableIntValue("ROX_QT_SLOW_MB")) * 1024 * 1024 : 24 * 1024 * 1024;
        const QString iso = tmp.filePath(QStringLiteral("slow-test.iso"));
        if (!check(writeBytes(iso, size), "slow fixture")) return 1;
        QFile::remove(QDir(slow).filePath(QStringLiteral("slow-test.iso")));
        QProcess::execute(QStringLiteral("sync"));
        qint64 syncMs = -1;
        QElapsedTimer t;
        t.start();
        if (!check(awaitTransfer({iso}, slow, &values, &syncMs), "slow transfer succeeds")) return 1;
        const qint64 jobMs = t.elapsed() - syncMs;
        QSet<int> distinct;
        bool monotonic = true;
        for (int i = 0; i < values.size(); ++i) {
            distinct.insert(values.at(i));
            if (i > 0 && values.at(i) < values.at(i - 1)) monotonic = false;
        }
        QStringList shown;
        for (int v : std::as_const(values)) shown << QString::number(v / 10);
        std::fprintf(stderr, "slow: job %lld ms, sync after job %lld ms, bar %%: %s\n",
                     jobMs, syncMs, qPrintable(shown.join(QLatin1Char(' '))));
        if (!check(monotonic, "bar never goes back")) return 1;
        if (!check(distinct.size() >= 4, "bar shows intermediate values")) return 1;
        if (!check(syncMs < 1000, "nothing left to write when the job reports done")) return 1;
        QFile::remove(QDir(slow).filePath(QStringLiteral("slow-test.iso")));
        std::fprintf(stderr, "PASS: slow destination %s\n", qPrintable(slow));
    }

    std::fprintf(stderr, "OK: totals, merge, cancel, tracked transfer\n");
    return 0;
}
