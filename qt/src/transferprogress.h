#pragma once

// Real byte progress for copy/move jobs (1.0.7).
//
// cp(1) returns as soon as the data is in the kernel page cache; on a slow
// USB stick the kernel then keeps writing for a long time after the copy
// window has closed.  TransferProgress runs beside the unchanged cp/mv
// commands and reports only bytes that sync_file_range() has confirmed on
// the device, then calls syncfs() on each destination filesystem.  The job is
// reported finished only after that, so 100% means "safe to remove".

#include <QHash>
#include <QList>
#include <QMutex>
#include <QPair>
#include <QString>
#include <QThread>
#include <QWaitCondition>

#include <atomic>
#include <ctime>
#include <sys/types.h>

class TransferProgress final : public QThread
{
    Q_OBJECT
public:
    struct Item {
        QString source;       // what cp/mv reads
        QString destination;  // the file or folder cp/mv writes
    };

    explicit TransferProgress(const QList<Item> &items, QObject *parent = nullptr);
    ~TransferProgress() override;

    qint64 totalBytes() const { return m_total.load(); }  // -1 while counting
    qint64 doneBytes() const;                              // confirmed on the device
    bool isFlushing() const { return m_flushing.load(); }

    // The GUI calls this once every cp/mv process has exited: a final pass
    // confirms what is left, then syncfs() runs and flushed() is emitted.
    void commandsFinished();
    // Stop as soon as possible; flushed() is not emitted.
    void cancel();

signals:
    void counted(qint64 totalBytes);
    void flushed();

protected:
    void run() override;

private:
    using Key = QPair<quint64, quint64>;  // st_dev, st_ino

    qint64 countSources();
    void scan();
    void confirmPath(const QString &path);
    void confirmFile(const QString &path);
    void flushDevices();
    void pause(int ms);

    const QList<Item> m_items;
    const time_t m_startCtime;

    std::atomic<qint64> m_total{-1};
    std::atomic<qint64> m_done{0};
    std::atomic<bool> m_commandsDone{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_flushing{false};

    // Worker-thread only.
    QHash<Key, qint64> m_confirmed;
    QHash<quint64, QString> m_devicePaths;  // st_dev -> a path on it, for syncfs
    QHash<Key, QString> m_fusePaths;        // FUSE files get fdatasync() at the end
    qint64 m_visited = 0;                   // files seen in the current pass

    // FUSE block filesystems (exfat-fuse, ntfs-3g): the page cache says
    // nothing there, so progress is what the block device has received,
    // read live from /sys/dev/block/MAJ:MIN/stat.  Filled before counted()
    // and read-only afterwards, so the GUI thread may read it.
    struct DeviceCounter {
        QString statPath;
        quint64 baseSectors = 0;
    };
    QHash<quint64, DeviceCounter> m_fuseDevices;
    std::atomic<qint64> m_fuseLogical{0};   // bytes cp has written to FUSE files
    void prepareFuseDevices();
    qint64 fuseDeviceBytes() const;

    QMutex m_mutex;
    QWaitCondition m_wake;
};
