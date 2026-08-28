#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"
#include "remotefilemanager/core/OperationProgress.hpp"
#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/RemoteFileOperations.hpp"
#include "remotefilemanager/core/SecurePassword.hpp"
#include "remotefilemanager/core/Storage.hpp"
#include "remotefilemanager/core/TransferTypes.hpp"
#include "remotefilemanager/core/VolumeService.hpp"

#include <QByteArray>
#include <QObject>
#include <functional>
#include <memory>

namespace rfm::core
{
class RemoteTransferBackend;
}

namespace rfm::ssh
{

class SshSessionTransferTest;

class SshSession final : public QObject
{
    Q_OBJECT

  public:
    explicit SshSession(QObject* parent = nullptr);
    ~SshSession() override;
    // Thread-safe: ownership is moved into a private Qt event for this object's thread.
    void postVolumeAuthentication(quint64 operationId, quint64 authenticationToken,
                                  rfm::core::SecurePassword password);

  public slots:
    void connectToHost(rfm::core::ConnectionProfile profile, QString password);
    void confirmUnknownHost(bool accepted);
    void listDirectory(quint64 requestId, QString path);
    void listStorageVolumes(quint64 requestId);
    void probeStorageMounts(quint64 requestId);
    void operateVolume(rfm::core::VolumeOperationRequest request);
    void cancelVolumeAuthentication(quint64 operationId, quint64 authenticationToken);
    void createDirectory(quint64 id, QString parent, QString name);
    void renameEntry(quint64 id, QString source, QString newName);
    void moveEntries(quint64 id, QList<rfm::core::RemoteSelection> sources,
                     QString destinationDirectory);
    void copyEntries(quint64 id, QList<rfm::core::RemoteSelection> sources,
                     QString destinationDirectory);
    void removeEntries(quint64 id, QList<rfm::core::RemoteSelection> sources, bool recursive);
    void startTransfer(rfm::core::TransferRequest request);
    void pauseTransfer(quint64 id);
    void resumeTransfer(quint64 id);
    void cancelTransfer(quint64 id);
    void cancelRemoteOperation(quint64 id);
    void shutdownTransfers();
    void disconnectFromHost();

  signals:
    void hostKeyConfirmationRequired(QString host, QString fingerprint);
    void connected(QString initialPath, QList<rfm::core::RemoteEntry> entries);
    void directoryListed(quint64 requestId, QString path, QList<rfm::core::RemoteEntry> entries);
    void directoryListingFailed(quint64 requestId, QString path, QString error);
    void storageVolumesListed(quint64 requestId, QList<rfm::core::StorageVolume> volumes);
    void storageMountInfoFingerprint(quint64 requestId, QByteArray fingerprint);
    void storageMountsProbed(quint64 requestId, QByteArray fingerprint);
    void storageMountProbeFailed(quint64 requestId, QString error);
    void storageVolumeListingFailed(quint64 requestId, QString error);
    void volumeOperationFinished(rfm::core::VolumeOperationResult result);
    void operationFinished(rfm::core::RemoteOperationResult result);
    void operationUpdated(rfm::core::OperationProgress progress);
    void transferUpdated(rfm::core::TransferProgress progress);
    void transferRejected(quint64 id, QString error);
    void transferExecutorFailed(QString error);
    void transfersShutdown();
    void failed(QString message);
    void disconnected();

  protected:
    bool event(QEvent* event) override;

  private:
    using TransferBackendFactory =
        std::function<std::unique_ptr<rfm::core::RemoteTransferBackend>()>;

    class Impl;
    std::unique_ptr<Impl> m_impl;
    friend class SshSessionTransferTest;

    SshSession(TransferBackendFactory transferBackendFactory,
               std::function<bool()> transferConnectionAvailable, QObject* parent);

    void authenticateAndOpen();
    void processTransferStep();
    void scheduleTransferStep();
    void processCopyStep();
    void scheduleCopyStep();
    void processStorageScanStep();
    void scheduleStorageScanStep();
    void cancelStorageScan();
    void processStorageProbeStep();
    void scheduleStorageProbeStep();
    void cancelStorageProbe();
    void authenticateVolume(quint64 operationId, quint64 authenticationToken,
                            rfm::core::SecurePassword password);
    void processVolumeCommandStep();
    void scheduleVolumeCommandStep(bool activityAvailable = true);
    void startRemoteStorageScanner(quint64 requestId);
    void startPendingRemoteWork();
    void completeShutdownIfReady();
    void publishTransferProgress(const rfm::core::TransferProgress& progress);
    void terminalizeTransfer(rfm::core::TransferState state, const QString& error);
    void fail(const QString& message);
};

} // namespace rfm::ssh
