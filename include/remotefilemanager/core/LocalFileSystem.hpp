#pragma once

#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/Storage.hpp"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

namespace rfm::core
{

struct LocalDirectoryResult {
    QString path;
    QList<RemoteEntry> entries;
    QString error;

    [[nodiscard]] bool succeeded() const { return error.isEmpty(); }
};

// Raw fields captured from one QStorageInfo enumeration. Keeping this value type
// separate makes it possible to derive the displayed volumes and their identity
// fingerprint from exactly the same mount snapshot.
struct LocalStorageMount {
    QString rootPath;
    QString device;
    QByteArray fileSystemType;
    QString fileSystemLabel;
    qint64 bytesTotal{0};
    bool readOnly{false};
    bool valid{false};
    bool ready{false};
};

struct LocalStorageSnapshot {
    QList<StorageVolume> volumes;
    QByteArray fingerprint;
};

class LocalFileSystem final
{
  public:
    [[nodiscard]] static LocalDirectoryResult listDirectory(const QString& path);
    [[nodiscard]] static QList<StorageVolume> mountedVolumes();
    [[nodiscard]] static LocalStorageSnapshot mountedVolumeSnapshot();
    [[nodiscard]] static QByteArray mountedVolumeFingerprint();
    [[nodiscard]] static LocalStorageSnapshot
    makeStorageSnapshot(const QList<LocalStorageMount>& mounts);
};

class LocalFileSystemWorker final : public QObject
{
    Q_OBJECT

  public slots:
    void listDirectory(quint64 requestId, QString path);
    void listVolumes();
    void probeVolumes(quint64 requestId);

  signals:
    void directoryListed(quint64 requestId, QString path, QList<rfm::core::RemoteEntry> entries);
    void directoryListingFailed(quint64 requestId, QString path, QString error);
    void volumesListed(QList<rfm::core::StorageVolume> volumes, QByteArray fingerprint);
    void volumesProbed(quint64 requestId, QByteArray fingerprint);
};

} // namespace rfm::core
