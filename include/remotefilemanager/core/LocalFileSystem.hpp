#pragma once

#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/Storage.hpp"

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <utility>

namespace rfm::core
{

struct LocalDirectoryResult {
    QString path;
    QList<RemoteEntry> entries;
    QString error;

    [[nodiscard]] bool succeeded() const { return error.isEmpty(); }
};

enum class LocalFileOperationKind { CreateDirectory, Rename, Remove, Copy, Move };

enum class LocalCollisionPolicy { Fail, Overwrite, Skip, Cancel };

enum class LocalFileOperationOutcome { Succeeded, Failed, Skipped, Collision, Cancelled };

enum class LocalRenameError { None, CrossDevice, Failure };

struct LocalRenameResult {
    LocalRenameError error{LocalRenameError::None};
    QString detail;

    [[nodiscard]] bool succeeded() const { return error == LocalRenameError::None; }
};

class LocalFileOperationBackend
{
  public:
    virtual ~LocalFileOperationBackend() = default;

    [[nodiscard]] virtual LocalRenameResult rename(const QString& source,
                                                   const QString& destination) = 0;
    [[nodiscard]] virtual bool validateCopy(const QString& source, const QString& destination,
                                            QString& error) = 0;
};

struct LocalFileOperationRequest {
    LocalFileOperationRequest() = default;
    LocalFileOperationRequest(quint64 operationId, LocalFileOperationKind operationKind,
                              QString operationParentPath, QStringList operationSourcePaths,
                              QString operationNewName, QString operationDestinationDirectory = {},
                              LocalCollisionPolicy operationCollisionPolicy =
                                  LocalCollisionPolicy::Fail)
        : id(operationId), kind(operationKind), parentPath(std::move(operationParentPath)),
          sourcePaths(std::move(operationSourcePaths)), newName(std::move(operationNewName)),
          destinationDirectory(std::move(operationDestinationDirectory)),
          collisionPolicy(operationCollisionPolicy)
    {}

    quint64 id{0};
    LocalFileOperationKind kind{LocalFileOperationKind::CreateDirectory};
    QString parentPath;
    QStringList sourcePaths;
    QString newName;
    QString destinationDirectory;
    LocalCollisionPolicy collisionPolicy{LocalCollisionPolicy::Fail};
};

struct LocalFileOperationItemResult {
    QString source;
    QString destination;
    bool success{false};
    QString error;
    LocalFileOperationOutcome outcome{LocalFileOperationOutcome::Failed};
};

struct LocalFileOperationResult {
    quint64 id{0};
    LocalFileOperationKind kind{LocalFileOperationKind::CreateDirectory};
    QList<LocalFileOperationItemResult> items;
    bool cancelled{false};

    [[nodiscard]] bool allSucceeded() const;
    [[nodiscard]] qsizetype succeededCount() const;
    [[nodiscard]] qsizetype failedCount() const;
    [[nodiscard]] qsizetype skippedCount() const;
};

// Raw fields captured from one QStorageInfo enumeration. Keeping this value type
// separate makes it possible to derive the displayed volumes and their identity
// fingerprint from exactly the same mount snapshot.
struct LocalStorageMount {
    LocalStorageMount() = default;
    LocalStorageMount(QString rootPathValue, QString deviceValue, QByteArray fileSystemTypeValue,
                      QString fileSystemLabelValue, qint64 bytesTotalValue, bool readOnlyValue,
                      bool validValue, bool readyValue, QString deviceNumberValue = {})
        : rootPath(std::move(rootPathValue)), device(std::move(deviceValue)),
          fileSystemType(std::move(fileSystemTypeValue)),
          fileSystemLabel(std::move(fileSystemLabelValue)), bytesTotal(bytesTotalValue),
          readOnly(readOnlyValue), valid(validValue), ready(readyValue),
          deviceNumber(std::move(deviceNumberValue))
    {}

    QString rootPath;
    QString device;
    QByteArray fileSystemType;
    QString fileSystemLabel;
    qint64 bytesTotal{0};
    bool readOnly{false};
    bool valid{false};
    bool ready{false};
    QString deviceNumber;
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
    // Returns true only when both parent filesystems can be identified reliably and differ.
    // A false result also covers unavailable filesystem metadata.
    [[nodiscard]] static bool pathsUseDifferentFileSystems(const QString& source,
                                                           const QString& destination);
    [[nodiscard]] static LocalDirectoryResult listDirectory(const QString& path);
    [[nodiscard]] static LocalFileOperationResult
    executeOperation(const LocalFileOperationRequest& request,
                     LocalFileOperationBackend* backend = nullptr,
                     const std::function<bool()>& cancellationRequested = {});
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
    void countDirectoryEntries(quint64 requestId, QString path);
    void listVolumes();
    void probeVolumes(quint64 requestId);

  signals:
    void directoryListed(quint64 requestId, QString path, QList<rfm::core::RemoteEntry> entries);
    void directoryListingFailed(quint64 requestId, QString path, QString error);
    void directoryCounted(quint64 requestId, QString path, quint64 count);
    void directoryCountFailed(quint64 requestId, QString path);
    void volumesListed(QList<rfm::core::StorageVolume> volumes, QByteArray fingerprint);
    void volumesProbed(quint64 requestId, QByteArray fingerprint);
};

class LocalFileOperationWorker final : public QObject
{
    Q_OBJECT

  public:
    // Thread-safe. It can be called directly while execute() owns the worker thread.
    void requestCancellation(quint64 operationId) noexcept;

  public slots:
    void execute(rfm::core::LocalFileOperationRequest request);

  signals:
    void started(quint64 operationId);
    void finished(rfm::core::LocalFileOperationResult result);

  private:
    mutable QMutex m_cancellationMutex;
    QSet<quint64> m_cancelledOperationIds;
};

} // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::LocalFileOperationKind)
Q_DECLARE_METATYPE(rfm::core::LocalCollisionPolicy)
Q_DECLARE_METATYPE(rfm::core::LocalFileOperationOutcome)
Q_DECLARE_METATYPE(rfm::core::LocalFileOperationRequest)
Q_DECLARE_METATYPE(rfm::core::LocalFileOperationResult)
