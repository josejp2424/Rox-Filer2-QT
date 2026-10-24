#include "transferprogress.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace {

// Small enough that a 4 MB/s stick still reports twice per second, large
// enough that a fast SSD does not drown in syscalls.
constexpr off_t kChunk = 2 * 1024 * 1024;
constexpr int kPollMs = 150;
constexpr unsigned long kFuseMagic = 0x65735546UL;

QByteArray nativePath(const QString &path) { return QFile::encodeName(path); }

bool lstatPath(const QString &path, struct stat &st)
{
    return ::lstat(nativePath(path).constData(), &st) == 0;
}

bool isFuse(const QString &path)
{
    struct statfs fs {};
    return ::statfs(nativePath(path).constData(), &fs) == 0
        && static_cast<unsigned long>(fs.f_type) == kFuseMagic;
}

// Field 7 of /sys/dev/block/MAJ:MIN/stat: 512-byte sectors written.
bool readSectorsWritten(const QString &statPath, quint64 *sectors)
{
    QFile f(statPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QList<QByteArray> fields = f.readAll().simplified().split(' ');
    if (fields.size() < 7)
        return false;
    bool ok = false;
    *sectors = fields.at(6).toULongLong(&ok);
    return ok;
}

} // namespace

TransferProgress::TransferProgress(const QList<Item> &items, QObject *parent)
    : QThread(parent), m_items(items),
      // Files cp creates or truncates get a ctime >= now.  Older files in a
      // merged destination folder are not part of this job.  The slack
      // covers FAT/exFAT timestamp granularity.
      m_startCtime(::time(nullptr) - 2)
{
}

TransferProgress::~TransferProgress()
{
    cancel();
    wait();
}

qint64 TransferProgress::doneBytes() const
{
    const qint64 total = m_total.load();
    qint64 done = m_done.load();
    if (!m_fuseDevices.isEmpty() && total >= 0)
        done += qMin(m_fuseLogical.load(), fuseDeviceBytes());
    // Hard links and sparse files can make the two sums differ slightly.
    return total >= 0 ? qBound<qint64>(0, done, total) : 0;
}

void TransferProgress::commandsFinished()
{
    m_commandsDone = true;
    QMutexLocker lock(&m_mutex);
    m_wake.wakeAll();
}

void TransferProgress::cancel()
{
    m_cancel = true;
    QMutexLocker lock(&m_mutex);
    m_wake.wakeAll();
}

void TransferProgress::pause(int ms)
{
    QMutexLocker lock(&m_mutex);
    if (!m_cancel.load() && !m_commandsDone.load())
        m_wake.wait(&m_mutex, ms);
}

void TransferProgress::prepareFuseDevices()
{
    for (const Item &item : m_items) {
        const QString parent = QFileInfo(item.destination).absolutePath();
        struct stat st {};
        if (!isFuse(parent) || ::stat(nativePath(parent).constData(), &st) != 0)
            continue;
        if (m_fuseDevices.contains(quint64(st.st_dev)))
            continue;
        // fuseblk reports the real block device; plain FUSE (sshfs) has an
        // anonymous 0:N device with no sysfs entry and keeps the fallback.
        DeviceCounter counter;
        counter.statPath = QStringLiteral("/sys/dev/block/%1:%2/stat")
                               .arg(major(st.st_dev)).arg(minor(st.st_dev));
        if (readSectorsWritten(counter.statPath, &counter.baseSectors))
            m_fuseDevices.insert(quint64(st.st_dev), counter);
    }
}

qint64 TransferProgress::fuseDeviceBytes() const
{
    qint64 bytes = 0;
    for (const DeviceCounter &counter : m_fuseDevices) {
        quint64 now = 0;
        if (readSectorsWritten(counter.statPath, &now) && now > counter.baseSectors)
            bytes += qint64(now - counter.baseSectors) * 512;
    }
    return bytes;
}

void TransferProgress::run()
{
    prepareFuseDevices();  // baseline before any cp/mv starts
    m_total = countSources();
    if (m_cancel.load())
        return;
    emit counted(m_total.load());

    // Confirm while cp/mv are running; one last full pass after they exit.
    for (;;) {
        const bool last = m_commandsDone.load();
        m_visited = 0;
        scan();
        if (m_cancel.load())
            return;
        if (last)
            break;
        // Big folder trees: walk them less often (4000 files -> 1 s).  Based
        // on the file count, not on elapsed time, which includes device waits.
        pause(int(qBound<qint64>(qint64(kPollMs), m_visited / 4, qint64(2000))));
    }

    m_flushing = true;
    flushDevices();
    if (m_cancel.load())
        return;
    m_done = m_total.load();
    emit flushed();
}

qint64 TransferProgress::countSources()
{
    qint64 total = 0;
    for (const Item &item : m_items) {
        struct stat st {};
        if (!lstatPath(item.source, st))
            continue;
        if (S_ISREG(st.st_mode)) {
            total += st.st_size;
        } else if (S_ISDIR(st.st_mode)) {
            // QDirIterator does not descend into symlinked folders unless
            // FollowSymlinks is given; cp -a copies those links as links.
            QDirIterator it(item.source,
                            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                            QDirIterator::Subdirectories);
            while (it.hasNext() && !m_cancel.load()) {
                const QString path = it.next();
                struct stat child {};
                if (lstatPath(path, child) && S_ISREG(child.st_mode))
                    total += child.st_size;
            }
        }
        if (m_cancel.load())
            break;
    }
    return total;
}

void TransferProgress::scan()
{
    for (const Item &item : m_items) {
        if (m_cancel.load())
            return;
        confirmPath(item.destination);
    }
}

void TransferProgress::confirmPath(const QString &path)
{
    struct stat st {};
    if (!lstatPath(path, st))
        return;  // cp has not created it yet
    if (S_ISREG(st.st_mode)) {
        confirmFile(path);
        return;
    }
    if (!S_ISDIR(st.st_mode))
        return;
    if (!m_devicePaths.contains(quint64(st.st_dev)))
        m_devicePaths.insert(quint64(st.st_dev), path);
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext() && !m_cancel.load())
        confirmFile(it.next());
}

void TransferProgress::confirmFile(const QString &path)
{
    ++m_visited;
    struct stat st {};
    if (!lstatPath(path, st) || !S_ISREG(st.st_mode))
        return;
    if (st.st_ctime < m_startCtime)
        return;  // untouched file that was already in the destination

    const Key key(quint64(st.st_dev), quint64(st.st_ino));
    qint64 confirmed = m_confirmed.value(key, 0);
    if (st.st_size < confirmed) {  // truncated and rewritten
        if (m_fusePaths.contains(key) && m_fuseDevices.contains(key.first))
            m_fuseLogical -= confirmed;
        else
            m_done -= confirmed;
        confirmed = 0;
    }
    if (!m_devicePaths.contains(key.first))
        m_devicePaths.insert(key.first, path);
    if (st.st_size == confirmed) {
        m_confirmed.insert(key, confirmed);
        return;
    }

    const int fd = ::open(nativePath(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        // Unreadable (e.g. a mode 0200 file): count it, it cannot be confirmed.
        m_done += st.st_size - confirmed;
        m_confirmed.insert(key, st.st_size);
        return;
    }
    struct stat now {};
    const off_t size = ::fstat(fd, &now) == 0 ? now.st_size : st.st_size;

    struct statfs fs {};
    if (::fstatfs(fd, &fs) == 0 && static_cast<unsigned long>(fs.f_type) == kFuseMagic) {
        // The data is already with the daemon; push it to the device
        // (FUSE_FSYNC) while cp continues.  With a block counter the bar
        // follows the device; without one this is the best available.
        m_fusePaths.insert(key, path);
        // Count what cp wrote first: the GUI shows min(written, device) live
        // while fdatasync() below blocks until the device has it.
        if (m_fuseDevices.contains(key.first))
            m_fuseLogical += size - confirmed;
        ::fdatasync(fd);
        if (!m_fuseDevices.contains(key.first))
            m_done += size - confirmed;
        ::close(fd);
        m_confirmed.insert(key, size);
        return;
    }

    while (confirmed < size && !m_cancel.load()) {
        const off_t n = qMin<off_t>(kChunk, size - confirmed);
        if (::sync_file_range(fd, confirmed, n,
                              SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE
                                  | SYNC_FILE_RANGE_WAIT_AFTER) != 0) {
            // Not supported here: fall back to a whole-file flush.
            ::fdatasync(fd);
            m_done += size - confirmed;
            confirmed = size;
            break;
        }
        confirmed += n;
        m_done += n;
    }
    ::close(fd);
    m_confirmed.insert(key, confirmed);
}

void TransferProgress::flushDevices()
{
    // FUSE (ntfs-3g, exfat-fuse) keeps its own buffers below the page cache;
    // fdatasync() reaches the daemon (FUSE_FSYNC), syncfs() may not.
    for (auto it = m_fusePaths.cbegin(); it != m_fusePaths.cend() && !m_cancel.load(); ++it) {
        const int fd = ::open(nativePath(it.value()).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) {
            ::fdatasync(fd);
            ::close(fd);
        }
    }
    // Directory entries, FAT tables, journal: only the destination filesystems.
    for (auto it = m_devicePaths.cbegin(); it != m_devicePaths.cend() && !m_cancel.load(); ++it) {
        const int fd = ::open(nativePath(it.value()).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd >= 0) {
            ::syncfs(fd);
            ::close(fd);
        }
    }
}
