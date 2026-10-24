#include "imagemounter.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace ImageMounter {

namespace {

// Same messages as GTK3 image_mounter.c, so its translations can be reused.
QString tr(const char *text)
{
    return QCoreApplication::translate("ImageMounter", text);
}

const char kGroup[] = "ImageMounter";

// 2.12.2-85 (GTK3): privileged helpers are resolved by absolute path in the
// usual system directories only.  A writable directory early in $PATH must
// never be able to run code as root.
const char *const kHelperDirs[] = {
    "/sbin", "/usr/sbin", "/bin", "/usr/bin", "/usr/local/sbin", "/usr/local/bin"
};

bool runSync(const QStringList &argv, QString *out, QString *err)
{
    if (out) out->clear();
    if (err) err->clear();
    if (argv.isEmpty())
        return false;
    const QString program = detail::helperPath(argv.first());
    if (program.isEmpty()) {
        if (err) *err = tr("Required program '%1' was not found.").arg(argv.first());
        return false;
    }
    QProcess process;
    process.setProgram(program);
    process.setArguments(argv.mid(1));
    process.start();
    if (!process.waitForStarted(15000)) {
        if (err) *err = process.errorString();
        return false;
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(180000)) {
        process.kill();
        process.waitForFinished(3000);
        if (err) *err = tr("Unknown error");
        return false;
    }
    const QString stdoutText = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    const QString stderrText = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (out) *out = stdoutText;
    if (err) *err = stderrText;
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool programExists(const QString &name)
{
    return !detail::helperPath(name).isEmpty();
}

// g_mkdir_with_parents(): every missing component is created with `mode`.
bool mkdirWithParents(const QString &path, mode_t mode)
{
    const QString clean = QDir::cleanPath(path);
    QString current;
    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        current += QLatin1Char('/') + part;
        const QByteArray encoded = QFile::encodeName(current);
        struct stat st {};
        if (::stat(encoded.constData(), &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                return false;
            }
            continue;
        }
        if (::mkdir(encoded.constData(), mode) != 0 && errno != EEXIST)
            return false;
    }
    return true;
}

QString readProcMounts()
{
    // readAll(): /proc files report size 0, so a readLine loop guarded by atEnd reads
    // nothing at all (this was the 1.0.4 bug).
    QFile file(QStringLiteral("/proc/self/mounts"));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromLocal8Bit(file.readAll());
}

bool suffixIs(const QString &path, const char *suffix)
{
    return path.endsWith(QLatin1String(suffix), Qt::CaseInsensitive);
}

QString blkidValue(const QString &device, const QString &field)
{
    if (!programExists(QStringLiteral("blkid")))
        return QString();
    QString out;
    if (!runSync({QStringLiteral("blkid"), QStringLiteral("-p"), QStringLiteral("-o"), QStringLiteral("value"),
                  QStringLiteral("-s"), field, device}, &out, nullptr))
        return QString();
    return out;
}

quint64 blockSizeBytes(const QString &device)
{
    QFile file(QStringLiteral("/sys/class/block/") + QFileInfo(device).fileName() + QStringLiteral("/size"));
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    return QString::fromLatin1(file.readAll()).trimmed().toULongLong() * 512ULL;
}

uint partitionNumber(const QString &device)
{
    const int p = device.lastIndexOf(QLatin1Char('p'));
    if (p < 0 || p + 1 >= device.size() || !device.at(p + 1).isDigit())
        return 0;
    return device.mid(p + 1).toUInt();
}

QStringList loopPartitions(const QString &loopdev)
{
    QStringList parts;
    const QString loopName = QFileInfo(loopdev).fileName();
    const QDir dir(QStringLiteral("/sys/class/block/") + loopName);
    if (!dir.exists())
        return parts;
    for (const QString &entry : dir.entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)) {
        if (!entry.startsWith(loopName) || entry.size() <= loopName.size() + 1)
            continue;
        if (entry.at(loopName.size()) != QLatin1Char('p') || !entry.at(loopName.size() + 1).isDigit())
            continue;
        const QString dev = QStringLiteral("/dev/") + entry;
        if (QFileInfo::exists(dev))
            parts << dev;
    }
    std::sort(parts.begin(), parts.end(), [](const QString &a, const QString &b) {
        const uint na = partitionNumber(a);
        const uint nb = partitionNumber(b);
        return na != nb ? na < nb : a < b;
    });
    return parts;
}

// A partitioned IMG may expose p1 before p2/p3 appear in sysfs: wait until the
// complete set is unchanged for five polls (~400 ms), like GTK3.
QStringList waitForPartitions(const QString &loopdev)
{
    QStringList stable;
    bool haveStable = false;
    int stableChecks = 0;
    for (int tries = 0; tries < 20; ++tries) {
        const QStringList current = loopPartitions(loopdev);
        if (haveStable && current == stable) {
            ++stableChecks;
        } else {
            stable = current;
            haveStable = true;
            stableChecks = 1;
        }
        if (!stable.isEmpty() && stableChecks >= 5)
            break;
        QThread::msleep(100);
    }
    return stable;
}

QString partitionDescription(const QString &device, int index)
{
    QString fstype = blkidValue(device, QStringLiteral("TYPE"));
    const QString label = blkidValue(device, QStringLiteral("LABEL"));
    const quint64 size = blockSizeBytes(device);
    const QString sizeText = size ? QLocale().formattedDataSize(qint64(size), 1, QLocale::DataSizeSIFormat)
                                  : QString();
    uint number = partitionNumber(device);
    if (number == 0)
        number = uint(index + 1);
    if (fstype.isEmpty())
        fstype = tr("Unknown filesystem");
    if (!label.isEmpty() && !sizeText.isEmpty())
        return tr("Partition %1 — %2 — %3 — %4").arg(number).arg(fstype, sizeText, label);
    if (!label.isEmpty())
        return tr("Partition %1 — %2 — %3").arg(number).arg(fstype, label);
    if (!sizeText.isEmpty())
        return tr("Partition %1 — %2 — %3").arg(number).arg(fstype, sizeText);
    return tr("Partition %1 — %2").arg(number).arg(fstype);
}

QString setupLoopRoot(const QString &image, QString *errorText)
{
    if (!programExists(QStringLiteral("losetup"))) {
        *errorText = tr("Required program '%1' was not found.").arg(QStringLiteral("losetup"));
        return QString();
    }
    QString out, err, loopdev;
    // Modern util-linux: one atomic read-only setup with partition scan.
    if (runSync({QStringLiteral("losetup"), QStringLiteral("--find"), QStringLiteral("--show"),
                 QStringLiteral("--read-only"), QStringLiteral("--partscan"), image}, &out, &err)) {
        loopdev = detail::extractLoopDevice(out);
    }
    // Older Puppy releases may provide a small losetup without --show or
    // --partscan: keep the historical two-step setup (GTK3 fallback).
    if (loopdev.isEmpty()) {
        QString freeOut, freeErr;
        if (runSync({QStringLiteral("losetup"), QStringLiteral("-f")}, &freeOut, &freeErr) &&
            freeOut.startsWith(QStringLiteral("/dev/loop"))) {
            QString setupErr;
            if (runSync({QStringLiteral("losetup"), QStringLiteral("-r"), freeOut, image}, nullptr, &setupErr)) {
                loopdev = freeOut;
            } else if (runSync({QStringLiteral("losetup"), freeOut, image}, nullptr, &setupErr)) {
                // BusyBox variants without -r: request kernel RO state; mount
                // still gets -o ro.
                if (programExists(QStringLiteral("blockdev")))
                    runSync({QStringLiteral("blockdev"), QStringLiteral("--setro"), freeOut}, nullptr, nullptr);
                loopdev = freeOut;
            }
            if (loopdev.isEmpty() && !setupErr.isEmpty())
                err = setupErr;
        }
        if (loopdev.isEmpty() && !freeErr.isEmpty())
            err = freeErr;
    }
    if (loopdev.isEmpty())
        *errorText = !err.isEmpty() ? err : tr("Unable to determine the loop device created for this image.");
    return loopdev;
}

QString setupLoopUdisks(const QString &image, QString *errorText)
{
    QString out, err;
    if (!runSync({QStringLiteral("udisksctl"), QStringLiteral("loop-setup"), QStringLiteral("--read-only"),
                  QStringLiteral("--file"), image}, &out, &err)) {
        *errorText = !err.isEmpty() ? err : tr("Unknown error");
        return QString();
    }
    const QString loopdev = detail::extractLoopDevice(out);
    if (loopdev.isEmpty())
        *errorText = tr("Unable to determine the loop device created for this image.");
    return loopdev;
}

void detachLoopRoot(const QString &loopdev)
{
    if (!loopdev.isEmpty() && programExists(QStringLiteral("losetup")))
        runSync({QStringLiteral("losetup"), QStringLiteral("-d"), loopdev}, nullptr, nullptr);
}

void detachLoopUdisks(const QString &loopdev)
{
    if (!loopdev.isEmpty() && programExists(QStringLiteral("udisksctl")))
        runSync({QStringLiteral("udisksctl"), QStringLiteral("loop-delete"), QStringLiteral("--block-device"),
                 loopdev}, nullptr, nullptr);
}

QString mountRootBlock(const QString &image, const QString &blockdev, QString *errorText)
{
    if (!programExists(QStringLiteral("mount"))) {
        *errorText = tr("Required program '%1' was not found.").arg(QStringLiteral("mount"));
        return QString();
    }
    const QString root = QString::fromLatin1(detail::kRootDir);
    if (!mkdirWithParents(root, 0755)) {
        *errorText = tr("Unable to create mount directory '%1': %2")
                         .arg(root, QString::fromLocal8Bit(std::strerror(errno)));
        return QString();
    }
    const QString mountpoint = root + QLatin1Char('/') + detail::safeMountName(image, blockdev);
    if (!mkdirWithParents(mountpoint, 0755)) {
        *errorText = tr("Unable to create mount directory '%1': %2")
                         .arg(mountpoint, QString::fromLocal8Bit(std::strerror(errno)));
        return QString();
    }
    QString err;
    if (!runSync({QStringLiteral("mount"), QStringLiteral("-o"), QStringLiteral("ro,nosuid,nodev"),
                  blockdev, mountpoint}, nullptr, &err)) {
        *errorText = !err.isEmpty() ? err : tr("Unknown error");
        QDir().rmdir(mountpoint); // non-recursive: only an empty directory goes
        return QString();
    }
    return mountpoint;
}

QString mountUdisksBlock(const QString &blockdev, QString *errorText)
{
    QString out, err;
    if (!runSync({QStringLiteral("udisksctl"), QStringLiteral("mount"), QStringLiteral("--block-device"),
                  blockdev}, &out, &err)) {
        *errorText = !err.isEmpty() ? err : tr("Unknown error");
        return QString();
    }
    // Prefer the kernel's view over localized udisksctl output.
    QString mountpoint = detail::findMountpointForDevice(blockdev);
    if (mountpoint.isEmpty()) {
        const int at = out.indexOf(QStringLiteral(" at "));
        if (at >= 0) {
            mountpoint = out.mid(at + 4).trimmed();
            if (mountpoint.endsWith(QLatin1Char('.')))
                mountpoint.chop(1);
        }
    }
    if (mountpoint.isEmpty())
        *errorText = tr("The image was mounted, but its mount point could not be found.");
    return mountpoint;
}

void removeStateFile(const QString &image)
{
    const QString path = detail::statePathForImage(image);
    if (!path.isEmpty())
        QFile::remove(path);
}

bool loadState(const QString &image, Managed *state)
{
    const QString path = detail::statePathForImage(image);
    return !path.isEmpty() && detail::loadStateFile(path, state);
}

void reportError(QWidget *parent, const QString &message)
{
    QMessageBox::warning(parent, tr("Image Mounter"), message);
}

// Work runs in a worker thread; `then` runs afterwards on the GUI thread.
void runInThread(std::function<void()> work, std::function<void()> then)
{
    QThread *thread = QThread::create(std::move(work));
    QObject::connect(thread, &QThread::finished, qApp, [thread, then = std::move(then)] {
        thread->deleteLater();
        then();
    });
    thread->start();
}

QProgressDialog *progressDialog(QWidget *parent, const QString &text)
{
    auto *dialog = new QProgressDialog(text, QString(), 0, 0, parent);
    dialog->setWindowTitle(tr("Image Mounter"));
    dialog->setCancelButton(nullptr);
    dialog->setMinimumDuration(0);
    dialog->setWindowModality(parent ? Qt::WindowModal : Qt::ApplicationModal);
    dialog->setAutoClose(false);
    dialog->setAutoReset(false);
    dialog->show();
    return dialog;
}

QString choosePartition(QWidget *parent, const QStringList &parts, const QStringList &labels)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(tr("Image Mounter"));
    dialog.setMinimumWidth(520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(tr("This image contains multiple partitions. Choose the partition to mount:"), &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto *combo = new QComboBox(&dialog);
    combo->addItems(labels);
    combo->setCurrentIndex(0);
    layout->addWidget(combo);
    auto *buttons = new QDialogButtonBox(&dialog);
    buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    buttons->addButton(tr("Mount"), QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return QString();
    const int active = combo->currentIndex();
    return active >= 0 && active < parts.size() ? parts.at(active) : QString();
}

struct MountJob
{
    QString image;
    Method method = Method::None;
    QString loopdev;
    QString blockdev;
    QString mountpoint;
    QString errorText;
    QStringList parts;
    QStringList labels;
};

// Images with a job in flight: a second click must not create a second loop.
QSet<QString> &busyImages()
{
    static QSet<QString> busy;
    return busy;
}

/* ---------------- phase 1: loop + partitions (worker) ---------------- */
void mountPhase1(MountJob &job)
{
    if (::geteuid() == 0) {
        job.method = Method::Root;
        job.loopdev = setupLoopRoot(job.image, &job.errorText);
    } else if (programExists(QStringLiteral("udisksctl"))) {
        job.method = Method::Udisks;
        job.loopdev = setupLoopUdisks(job.image, &job.errorText);
    } else {
        job.errorText = QCoreApplication::translate(
            "FileOps", "Image mounting requires root privileges or udisksctl (udisks2).");
        return;
    }
    if (job.loopdev.isEmpty())
        return;

    // ISO and SquashFS images are complete filesystem images even when a
    // hybrid ISO also exposes a partition table: mount the WHOLE loop device.
    // Only raw .img files descend into loopNpM.
    if (!detail::isPartitionableRaw(job.image)) {
        job.blockdev = job.loopdev;
        return;
    }
    // A raw .img may contain one filesystem directly, with no partition table.
    if (programExists(QStringLiteral("blkid")) &&
        blkidValue(job.loopdev, QStringLiteral("PTTYPE")).isEmpty()) {
        job.blockdev = job.loopdev;
        return;
    }
    QStringList parts = waitForPartitions(job.loopdev);
    if (parts.isEmpty() && job.method == Method::Root && programExists(QStringLiteral("partx"))) {
        runSync({QStringLiteral("partx"), QStringLiteral("-a"), job.loopdev}, nullptr, nullptr);
        parts = waitForPartitions(job.loopdev);
    }
    if (parts.isEmpty()) {
        job.blockdev = job.loopdev;
        return;
    }
    if (parts.size() == 1) {
        job.blockdev = parts.first();
        return;
    }
    job.parts = parts;
    for (int i = 0; i < parts.size(); ++i)
        job.labels << partitionDescription(parts.at(i), i);
}

/* ---------------- phase 2: mount (worker) ---------------- */
void mountPhase2(MountJob &job)
{
    job.errorText.clear();
    if (job.method == Method::Root)
        job.mountpoint = mountRootBlock(job.image, job.blockdev, &job.errorText);
    else
        job.mountpoint = mountUdisksBlock(job.blockdev, &job.errorText);
}

void detachJobLoop(const MountJob &job)
{
    if (job.loopdev.isEmpty())
        return;
    if (job.method == Method::Root)
        detachLoopRoot(job.loopdev);
    else
        detachLoopUdisks(job.loopdev);
}

void finishJob(const std::shared_ptr<MountJob> &job, QPointer<QProgressDialog> progress)
{
    if (progress)
        progress->deleteLater();
    busyImages().remove(job->image);
}

// A failed mount detaches the loop device it created (nothing is mounted on
// it), off the GUI thread.
void abortJob(const std::shared_ptr<MountJob> &job, QPointer<QProgressDialog> progress,
              std::function<void()> afterwards = nullptr)
{
    runInThread([job] { detachJobLoop(*job); },
                [job, progress, afterwards] {
                    finishJob(job, progress);
                    if (afterwards) afterwards();
                });
}

void startPhase2(const std::shared_ptr<MountJob> &job, QPointer<QWidget> parent,
                 QPointer<QProgressDialog> progress, std::function<void(const QString &)> done)
{
    runInThread([job] { mountPhase2(*job); }, [job, parent, progress, done] {
        if (job->mountpoint.isEmpty()) {
            const QString message = tr("Unable to mount image '%1': %2")
                .arg(job->image, job->errorText.isEmpty() ? tr("Unknown error") : job->errorText);
            if (progress) progress->hide();
            abortJob(job, progress, [parent, message] { reportError(parent, message); });
            return;
        }
        Managed state;
        state.image = job->image;
        state.loopdev = job->loopdev;
        state.blockdev = job->blockdev;
        state.mountpoint = job->mountpoint;
        state.method = job->method;
        // 2.12.2-85 (GTK3): an image mounted without a state file could not be
        // unmounted from the interface; roll the mount back instead.
        if (!detail::saveState(state)) {
            runInThread([state] { QString ignored; detail::unmountStateSync(state, &ignored); },
                        [job, parent, progress] {
                            finishJob(job, progress);
                            reportError(parent, tr("The image was mounted, but its state could not be saved, "
                                                   "so it was unmounted again: '%1'").arg(job->image));
                        });
            return;
        }
        finishJob(job, progress);
        if (done) done(job->mountpoint);
    });
}

} // namespace

/* ======================================================== detail */

namespace detail {

QString helperPath(const QString &name)
{
    if (name.isEmpty() || name.contains(QLatin1Char('/')))
        return QString();
    for (const char *dir : kHelperDirs) {
        const QString candidate = QString::fromLatin1(dir) + QLatin1Char('/') + name;
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable())
            return candidate;
    }
    return QString();
}

QString canonicalImagePath(const QString &path)
{
    // g_canonicalize_filename(): absolute and without ./.., symlinks NOT
    // resolved.  It must match GTK3 so both builds hash to the same file.
    if (path.isEmpty())
        return QString();
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString stateDirectory()
{
    // Use XDG_RUNTIME_DIR only when the session really provides it; the state
    // is transient loop/mount bookkeeping.
    const QString runtime = QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR"));
    const QString base = !runtime.isEmpty() && QDir::isAbsolutePath(runtime)
        ? runtime
        : QDir::tempPath() + QStringLiteral("/rox-filer2-%1").arg(qulonglong(::getuid()));
    const QString dir = base + QStringLiteral("/rox-filer2/image-mounter");
    if (!mkdirWithParents(dir, 0700))
        return QString();
    ::chmod(QFile::encodeName(dir).constData(), 0700);
    return dir;
}

QString statePathForImage(const QString &image)
{
    const QString canonical = canonicalImagePath(image);
    if (canonical.isEmpty())
        return QString();
    const QString dir = stateDirectory();
    if (dir.isEmpty())
        return QString();
    const QByteArray hash = QCryptographicHash::hash(QFile::encodeName(canonical),
                                                     QCryptographicHash::Sha256).toHex();
    return dir + QLatin1Char('/') + QString::fromLatin1(hash) + QStringLiteral(".ini");
}

// GKeyFile string escaping (g_key_file_parse_string_as_value and back).
QString keyFileEscape(const QString &value)
{
    QString out;
    for (int i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c == QLatin1Char(' ') && i == 0) out += QStringLiteral("\\s");
        else if (c == QLatin1Char('\n')) out += QStringLiteral("\\n");
        else if (c == QLatin1Char('\t')) out += QStringLiteral("\\t");
        else if (c == QLatin1Char('\r')) out += QStringLiteral("\\r");
        else if (c == QLatin1Char('\\')) out += QStringLiteral("\\\\");
        else out += c;
    }
    return out;
}

QString keyFileUnescape(const QString &value)
{
    QString out;
    for (int i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c == QLatin1Char('\\') && i + 1 < value.size()) {
            const QChar n = value.at(++i);
            if (n == QLatin1Char('s')) out += QLatin1Char(' ');
            else if (n == QLatin1Char('n')) out += QLatin1Char('\n');
            else if (n == QLatin1Char('t')) out += QLatin1Char('\t');
            else if (n == QLatin1Char('r')) out += QLatin1Char('\r');
            else if (n == QLatin1Char('\\')) out += QLatin1Char('\\');
            else { out += QLatin1Char('\\'); out += n; }
        } else {
            out += c;
        }
    }
    return out;
}

bool saveState(const Managed &state)
{
    const QString path = statePathForImage(state.image);
    if (path.isEmpty())
        return false;
    QByteArray data;
    data += "[" + QByteArray(kGroup) + "]\n";
    data += "image=" + keyFileEscape(state.image).toUtf8() + "\n";
    data += "loopdev=" + keyFileEscape(state.loopdev).toUtf8() + "\n";
    data += "blockdev=" + keyFileEscape(state.blockdev).toUtf8() + "\n";
    data += "mountpoint=" + keyFileEscape(state.mountpoint).toUtf8() + "\n";
    data += "method=" + QByteArray::number(int(state.method)) + "\n";
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(data) != data.size() || !file.commit())
        return false;
    ::chmod(QFile::encodeName(path).constData(), 0600);
    return true;
}

bool loadStateFile(const QString &statePath, Managed *state)
{
    *state = Managed();
    QFile file(statePath);
    if (statePath.isEmpty() || !file.open(QIODevice::ReadOnly))
        return false;
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    bool inGroup = false;
    bool haveImage = false, haveLoop = false, haveBlock = false, haveMount = false;
    int method = 0;
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1Char('['))) {
            inGroup = line == QStringLiteral("[") + QLatin1String(kGroup) + QStringLiteral("]");
            continue;
        }
        if (!inGroup)
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        QString value = line.mid(eq + 1);
        while (value.startsWith(QLatin1Char(' ')))
            value.remove(0, 1);
        value = keyFileUnescape(value);
        if (key == QLatin1String("image")) { state->image = value; haveImage = true; }
        else if (key == QLatin1String("loopdev")) { state->loopdev = value; haveLoop = true; }
        else if (key == QLatin1String("blockdev")) { state->blockdev = value; haveBlock = true; }
        else if (key == QLatin1String("mountpoint")) { state->mountpoint = value; haveMount = true; }
        else if (key == QLatin1String("method")) method = value.toInt();
    }
    state->method = method == 1 ? Method::Root : method == 2 ? Method::Udisks : Method::None;
    const bool ok = haveImage && haveLoop && haveBlock && haveMount && state->method != Method::None;
    if (!ok)
        *state = Managed();
    return ok;
}

QString unescapeMountField(const QString &field)
{
    QString out;
    for (int i = 0; i < field.size(); ++i) {
        if (field.at(i) == QLatin1Char('\\') && i + 3 < field.size()) {
            const QChar a = field.at(i + 1), b = field.at(i + 2), c = field.at(i + 3);
            if (a >= QLatin1Char('0') && a <= QLatin1Char('7') && b >= QLatin1Char('0') &&
                b <= QLatin1Char('7') && c >= QLatin1Char('0') && c <= QLatin1Char('7')) {
                out += QChar((a.unicode() - '0') * 64 + (b.unicode() - '0') * 8 + (c.unicode() - '0'));
                i += 3;
                continue;
            }
        }
        out += field.at(i);
    }
    return out;
}

bool mountpointIsActive(const QString &mountpoint)
{
    if (mountpoint.isEmpty())
        return false;
    const QStringList lines = readProcMounts().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList fields = line.split(QLatin1Char(' '));
        if (fields.size() >= 2 && unescapeMountField(fields.at(1)) == mountpoint)
            return true;
    }
    return false;
}

QString findMountpointForDevice(const QString &device)
{
    if (device.isEmpty())
        return QString();
    const QStringList lines = readProcMounts().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList fields = line.split(QLatin1Char(' '));
        if (fields.size() >= 2 && unescapeMountField(fields.at(0)) == device)
            return unescapeMountField(fields.at(1));
    }
    return QString();
}

// 2.12.2-85 (GTK3): every mount point we unmount or remove must be inside the
// tree we manage; a stale or corrupt state file can never point elsewhere.
bool mountpointIsOurs(const QString &mountpoint)
{
    const QString prefix = QString::fromLatin1(kRootDir) + QLatin1Char('/');
    if (mountpoint.isEmpty() || !mountpoint.startsWith(prefix))
        return false;
    if (mountpoint.contains(QStringLiteral("/../")) || mountpoint.endsWith(QStringLiteral("/..")))
        return false;
    return mountpoint.size() > prefix.size();
}

bool blockdevBelongsToLoop(const QString &loopdev, const QString &blockdev)
{
    if (loopdev.isEmpty() || blockdev.isEmpty())
        return false;
    if (loopdev == blockdev)
        return true;
    if (!blockdev.startsWith(loopdev))
        return false;
    const QString suffix = blockdev.mid(loopdev.size());
    if (suffix.size() < 2 || suffix.at(0) != QLatin1Char('p'))
        return false;
    for (int i = 1; i < suffix.size(); ++i)
        if (!suffix.at(i).isDigit())
            return false;
    return true;
}

bool isPartitionableRaw(const QString &image)
{
    return suffixIs(image, ".img");
}

QString safeMountName(const QString &image, const QString &blockdev)
{
    const QByteArray base = QFile::encodeName(QFileInfo(image).fileName());
    QByteArray safe;
    for (const char ch : base) {
        if (safe.size() >= 72)
            break;
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.')
            safe += char(c);
        else
            safe += '_';
    }
    if (safe.isEmpty())
        safe = "image";
    return QString::fromLatin1(safe) + QLatin1Char('-') + QFileInfo(blockdev).fileName();
}

QString extractLoopDevice(const QString &text)
{
    int from = 0;
    while (true) {
        const int p = text.indexOf(QStringLiteral("/dev/loop"), from);
        if (p < 0)
            return QString();
        int end = p + 9;
        while (end < text.size() && text.at(end).isDigit())
            ++end;
        if (end > p + 9)
            return text.mid(p, end - p);
        from = p + 1;
    }
}

bool unmountStateSync(const Managed &state, QString *error)
{
    error->clear();
    if (state.image.isEmpty() || state.mountpoint.isEmpty())
        return false;
    if (!mountpointIsActive(state.mountpoint))
        return true;
    if (state.method == Method::Root) {
        if (!mountpointIsOurs(state.mountpoint)) {
            *error = tr("Refusing to unmount '%1': it is outside the directory Rox-Filer2 manages for images (%2).")
                         .arg(state.mountpoint, QString::fromLatin1(kRootDir));
            return false;
        }
        if (!runSync({QStringLiteral("umount"), state.mountpoint}, nullptr, error))
            return false;
        // Detach only after umount really succeeded.
        detachLoopRoot(state.loopdev);
        QDir().rmdir(state.mountpoint);
        return true;
    }
    if (state.method == Method::Udisks) {
        if (!runSync({QStringLiteral("udisksctl"), QStringLiteral("unmount"), QStringLiteral("--block-device"),
                      state.blockdev}, nullptr, error))
            return false;
        detachLoopUdisks(state.loopdev);
        return true;
    }
    *error = tr("Unknown error");
    return false;
}

} // namespace detail

/* ======================================================== public API */

bool canHandle(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile())
        return false;
    for (const char *suffix : {".iso", ".sfs", ".squashfs", ".sqfs", ".sqsh", ".img"})
        if (suffixIs(path, suffix))
            return true;
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(info);
    for (const char *type : {"application/vnd.squashfs", "application/vnd.efi.iso", "application/x-cd-image",
                             "application/x-iso9660-image", "application/vnd.efi.img",
                             "application/x-raw-disk-image"})
        if (mime.inherits(QLatin1String(type)))
            return true;
    return false;
}

QString mountedPoint(const QString &image)
{
    Managed state;
    if (!loadState(image, &state))
        return QString();
    if (!detail::mountpointIsActive(state.mountpoint)) {
        removeStateFile(image);
        return QString();
    }
    return state.mountpoint;
}

QStringList mountPoints(const QString &image)
{
    const QString point = mountedPoint(image);
    return point.isEmpty() ? QStringList() : QStringList{point};
}

QList<Managed> managedMounts()
{
    QList<Managed> mounts;
    const QString dirPath = detail::stateDirectory();
    if (dirPath.isEmpty())
        return mounts;
    const QDir dir(dirPath);
    for (const QString &name : dir.entryList({QStringLiteral("*.ini")}, QDir::Files)) {
        const QString statePath = dir.filePath(name);
        Managed state;
        if (!detail::loadStateFile(statePath, &state))
            continue;
        // The private state directory is the authority.  Never enumerate the
        // system's loop devices: Puppy/Essora use loops for their own SFS.
        const bool validLoop = state.loopdev.startsWith(QStringLiteral("/dev/loop")) &&
                               detail::blockdevBelongsToLoop(state.loopdev, state.blockdev);
        if (!validLoop || !detail::mountpointIsActive(state.mountpoint)) {
            QFile::remove(statePath);
            continue;
        }
        mounts << state;
    }
    std::sort(mounts.begin(), mounts.end(), [](const Managed &a, const Managed &b) { return a.image < b.image; });
    return mounts;
}

QString signature(const QList<Managed> &mounts)
{
    QStringList parts;
    for (const Managed &m : mounts)
        parts << m.image + QLatin1Char('|') + m.blockdev + QLatin1Char('|') + m.mountpoint;
    return parts.join(QLatin1Char('\n'));
}

void mount(QWidget *parentWidget, const QString &path, std::function<void(const QString &)> done)
{
    QPointer<QWidget> parent(parentWidget);
    if (!canHandle(path)) {
        reportError(parent, tr("Unsupported image format."));
        return;
    }
    const QString canonical = detail::canonicalImagePath(path);
    if (canonical.isEmpty())
        return;
    const QString existing = mountedPoint(canonical);
    if (!existing.isEmpty()) {
        if (done) done(existing);
        return;
    }
    if (busyImages().contains(canonical))
        return;
    busyImages().insert(canonical);

    auto job = std::make_shared<MountJob>();
    job->image = canonical;
    QPointer<QProgressDialog> progress = progressDialog(parent, tr("Mounting image…"));
    runInThread([job] { mountPhase1(*job); }, [job, parent, progress, done] {
        if (job->loopdev.isEmpty()) {
            const QString message = tr("Unable to create a loop device for '%1': %2")
                .arg(job->image, job->errorText.isEmpty() ? tr("Unknown error") : job->errorText);
            finishJob(job, progress);
            reportError(parent, message);
            return;
        }
        if (job->parts.size() > 1) {
            if (progress) progress->hide();
            job->blockdev = choosePartition(parent, job->parts, job->labels);
            if (job->blockdev.isEmpty()) {
                abortJob(job, progress);
                return;
            }
            if (progress) progress->show();
        }
        if (job->blockdev.isEmpty())
            job->blockdev = job->loopdev;
        startPhase2(job, parent, progress, done);
    });
}

void unmount(QWidget *parentWidget, const QString &path, std::function<void(bool)> done)
{
    QPointer<QWidget> parent(parentWidget);
    const QString image = detail::canonicalImagePath(path);
    Managed state;
    if (!loadState(image, &state) || !detail::mountpointIsActive(state.mountpoint)) {
        removeStateFile(image);
        reportError(parent, tr("The mounted image is no longer available."));
        if (done) done(false);
        return;
    }
    if (state.method == Method::Root && !detail::mountpointIsOurs(state.mountpoint)) {
        removeStateFile(image);
        reportError(parent, tr("Refusing to unmount '%1': it is outside the directory Rox-Filer2 manages for images (%2).")
                                .arg(state.mountpoint, QString::fromLatin1(detail::kRootDir)));
        if (done) done(false);
        return;
    }
    QPointer<QProgressDialog> progress = progressDialog(parent, tr("Unmounting image…"));
    auto error = std::make_shared<QString>();
    auto ok = std::make_shared<bool>(false);
    runInThread([state, error, ok] { *ok = detail::unmountStateSync(state, error.get()); },
                [image, parent, progress, error, ok, done] {
                    if (progress) progress->deleteLater();
                    if (*ok) {
                        removeStateFile(image);
                    } else {
                        reportError(parent, tr("Unable to unmount image '%1': %2")
                                                .arg(image, error->isEmpty() ? tr("Unknown error") : *error));
                    }
                    if (done) done(*ok);
                });
}

} // namespace ImageMounter
