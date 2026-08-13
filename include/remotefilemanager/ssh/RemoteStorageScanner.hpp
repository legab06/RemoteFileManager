#pragma once

#include "remotefilemanager/core/Storage.hpp"

#include <QHash>
#include <QSet>

#include <memory>

namespace rfm::ssh
{

enum class RemoteStorageError {
    None,
    NotFound,
    PermissionDenied,
    ConnectionLost,
    Cancelled,
    OtherError
};

struct RemoteStorageByteResult {
    QByteArray data;
    RemoteStorageError error{RemoteStorageError::None};
    bool truncated{false};
};

struct RemoteStorageChunkResult {
    QByteArray data;
    RemoteStorageError error{RemoteStorageError::None};
    bool end{false};
};

struct RemoteStorageStringResult {
    QString value;
    RemoteStorageError error{RemoteStorageError::None};
};

struct RemoteStorageLabelResult {
    QString label;
    QString deviceIdentity;
    RemoteStorageError error{RemoteStorageError::None};
    bool end{false};
};

struct RemoteStorageTopologyResult {
    rfm::core::StorageTopologyNode node;
    RemoteStorageError error{RemoteStorageError::None};
    bool reliable{true};
};

struct RemoteStorageSizeResult {
    quint64 bytes{0};
    RemoteStorageError error{RemoteStorageError::None};
};

class RemoteStorageReader
{
  public:
    virtual ~RemoteStorageReader() = default;

    [[nodiscard]] virtual RemoteStorageError beginMountInfo() = 0;
    [[nodiscard]] virtual RemoteStorageChunkResult readMountInfoChunk(qsizetype maximumBytes) = 0;
    virtual void endMountInfo() = 0;
    [[nodiscard]] virtual RemoteStorageByteResult readFile(const QString& path,
                                                           qsizetype maximumBytes) = 0;
    [[nodiscard]] virtual RemoteStorageStringResult readLink(const QString& path) = 0;
    [[nodiscard]] virtual RemoteStorageError beginFileSystemLabels() = 0;
    [[nodiscard]] virtual RemoteStorageLabelResult nextFileSystemLabel() = 0;
    virtual void endFileSystemLabels() = 0;
    [[nodiscard]] virtual RemoteStorageTopologyResult readTopologyNode(const QString& path) = 0;
    [[nodiscard]] virtual RemoteStorageStringResult deviceIdentity(const QString& device) = 0;
    [[nodiscard]] virtual RemoteStorageSizeResult storageSize(const QString& mountPoint) = 0;
    [[nodiscard]] virtual bool connectionAlive() const = 0;
};

enum class RemoteStorageScanStatus { Pending, Completed, Failed, ConnectionLost, Cancelled };

struct RemoteStorageScanStep {
    RemoteStorageScanStatus status{RemoteStorageScanStatus::Pending};
    QString error;
};

class RemoteStorageScanner final
{
  public:
    struct Limits {
        qsizetype maximumMountInfoBytes{1024 * 1024};
        // One SFTP read per scanner step keeps cancellation cooperative.
        qsizetype maximumMountInfoChunkBytes{4096};
        qsizetype maximumMounts{128};
        qsizetype maximumLabels{128};
        // Total traversal work for the whole snapshot, not a depth guess.
        qsizetype maximumTopologyNodes{128};
    };

    RemoteStorageScanner(std::unique_ptr<RemoteStorageReader> reader, quint64 requestId);
    RemoteStorageScanner(std::unique_ptr<RemoteStorageReader> reader, quint64 requestId,
                         Limits limits);
    ~RemoteStorageScanner();

    [[nodiscard]] quint64 requestId() const;
    [[nodiscard]] RemoteStorageScanStep step();
    void cancel();
    [[nodiscard]] QList<rfm::core::StorageVolume> takeVolumes();
    [[nodiscard]] QByteArray mountInfoFingerprint() const;

  private:
    enum class Stage {
        BeginMountInfo,
        MountInfo,
        ParseMountInfo,
        BeginLabels,
        Labels,
        BeginMount,
        ResolveMountDevice,
        ReadDeviceNumber,
        ReadBlockDeviceLink,
        Topology,
        FinishMount,
        Completed,
        Failed,
        ConnectionLost,
        Cancelled
    };

    [[nodiscard]] RemoteStorageScanStep terminalStep() const;
    [[nodiscard]] RemoteStorageScanStep fail(RemoteStorageScanStatus status, const QString& error);
    [[nodiscard]] RemoteStorageScanStep handleRequiredError(RemoteStorageError error,
                                                            const QString& context);
    void closeMountInfo();
    void closeLabels();
    void resetCurrentMount();

    std::unique_ptr<RemoteStorageReader> m_reader;
    quint64 m_requestId{0};
    Limits m_limits;
    Stage m_stage{Stage::BeginMountInfo};
    QString m_error;
    QByteArray m_mountInfo;
    QByteArray m_mountInfoFingerprint;
    QList<rfm::core::LinuxMountInfo> m_mounts;
    QList<rfm::core::StorageVolume> m_volumes;
    QHash<QString, QString> m_labels;
    qsizetype m_labelCount{0};
    qsizetype m_mountIndex{0};
    rfm::core::LinuxMountInfo m_currentMount;
    QList<rfm::core::StorageTopologyNode> m_ancestry;
    QSet<QString> m_visitedTopologyPaths;
    QString m_currentTopologyPath;
    QString m_currentDeviceIdentity;
    QString m_currentDeviceNumber;
    qsizetype m_topologyNodeCount{0};
    bool m_mountInfoOpen{false};
    bool m_labelsOpen{false};
    bool m_currentBlockDevice{false};
    bool m_currentVirtualBlockDevice{false};
    bool m_currentTopologyComplete{false};
};

} // namespace rfm::ssh
