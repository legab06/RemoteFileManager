#pragma once

#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/Storage.hpp"

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

namespace rfm::core
{

struct LocalDirectoryResult {
    QString path;
    QList<RemoteEntry> entries;
    QString error;

    [[nodiscard]] bool succeeded() const { return error.isEmpty(); }
};

enum class LocalFileOperationKind { CreateDirectory, Rename, Remove };

struct LocalFileOperationRequest {
    quint64 id{0};
    LocalFileOperationKind kind{LocalFileOperationKind::CreateDirectory};
    QString parentPath;
    QStringList sourcePaths;
    QString newName;
};

struct LocalFileOperationItemResult {
    QString source;
    QString destination;
    bool success{false};
    QString error;
};

struct LocalFileOperationResult {
    quint64 id{0};
    LocalFileOperationKind kind{LocalFileOperationKind::CreateDirectory};
    QList<LocalFileOperationItemResult> items;

    [[nodiscard]] bool allSucceeded() const;
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

[[nodiscard]] bool localPathIsAtOrBelow(const QString& path, const QString& rootPath);

using LocalBlockDevice = LinuxBlockDevice;

class LocalFileSystem final
{
  public:
    [[nodiscard]] static bool isValidName(const QString& name);
    [[nodiscard]] static LocalDirectoryResult listDirectory(const QString& path);
    [[nodiscard]] static LocalFileOperationResult
    executeOperation(const LocalFileOperationRequest& request);
    [[nodiscard]] static QList<StorageVolume> mountedVolumes();
    [[nodiscard]] static LocalStorageSnapshot mountedVolumeSnapshot();
    [[nodiscard]] static QByteArray mountedVolumeFingerprint();
    [[nodiscard]] static QList<StorageVolume> storageVolumes();
    [[nodiscard]] static LocalStorageSnapshot storageSnapshot();
    [[nodiscard]] static QByteArray storageFingerprint();
    [[nodiscard]] static QList<LocalBlockDevice> parseLinuxBlockDevices(const QByteArray& output);
    [[nodiscard]] static LocalStorageSnapshot
    makeStorageSnapshot(const QList<LocalStorageMount>& mounts,
                        const QList<LocalBlockDevice>& blockDevices = {});
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

class LocalFileOperationWorker final : public QObject
{
    Q_OBJECT

  public slots:
    void execute(rfm::core::LocalFileOperationRequest request);

  signals:
    void finished(rfm::core::LocalFileOperationResult result);
};

} // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::LocalFileOperationKind)
Q_DECLARE_METATYPE(rfm::core::LocalFileOperationRequest)
Q_DECLARE_METATYPE(rfm::core::LocalFileOperationResult)
