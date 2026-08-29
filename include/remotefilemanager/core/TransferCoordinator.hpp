#pragma once

#include "remotefilemanager/core/OperationProgress.hpp"
#include "remotefilemanager/core/TransferQueue.hpp"

#include <QObject>
#include <QQueue>
#include <QSet>

#include <optional>

namespace rfm::core
{

class TransferCoordinator final : public QObject
{
    Q_OBJECT

  public:
    explicit TransferCoordinator(QObject* parent = nullptr);

  public slots:
    void executorConnected();
    void executorDisconnected();
    void enqueueTransfer(rfm::core::TransferRequest request);
    void enqueueRemoteOperation(rfm::core::RemoteOperationRequest request);
    void pauseTransfer(quint64 id);
    void resumeTransfer(quint64 id);
    void cancelTransfer(quint64 id);
    void cancelRemoteOperation(quint64 id);
    void handleExecutorProgress(rfm::core::TransferProgress progress);
    void handleRemoteExecutorProgress(rfm::core::OperationProgress progress);
    void handleRemoteExecutorResult(rfm::core::RemoteOperationResult result);
    void handleExecutorFailure(QString error);
    void handleRemoteExecutorFailure(QString error);
    void handleExecutorRejection(quint64 id, QString error);
    void shutdownTransfers();
    void disconnectExecutor();
    void handleExecutorShutdown();

  signals:
    void startTransferRequested(rfm::core::TransferRequest request);
    void pauseActiveRequested(quint64 id);
    void resumeActiveRequested(quint64 id);
    void cancelActiveRequested(quint64 id);
    void startRemoteOperationRequested(rfm::core::RemoteOperationRequest request);
    void cancelActiveRemoteOperationRequested(quint64 id);
    void shutdownExecutorRequested();
    void disconnectExecutorRequested();
    void transferUpdated(rfm::core::TransferProgress progress);
    void remoteOperationUpdated(rfm::core::OperationProgress progress);
    void remoteOperationFinished(rfm::core::RemoteOperationResult result);
    void remoteOperationResultSilent(quint64 id);
    void transferRejected(quint64 id, QString error);
    void remoteOperationRejected(quint64 id, QString error);
    void transfersShutdown();

  private:
    enum class StopMode { None, Shutdown, Disconnect };

    void dispatchNext();
    void dispatchNextRemoteOperation();
    void publishProgress(const rfm::core::TransferProgress& progress);
    void publishRemoteProgress(const rfm::core::OperationProgress& progress);
    void terminalizeQueued(rfm::core::TransferState state, const QString& error);
    void terminalizeQueuedRemoteOperations(rfm::core::OperationState state, const QString& error,
                                           bool suppressDialog = false);
    void terminalizeRemoteOperation(const rfm::core::RemoteOperationRequest& request,
                                    rfm::core::OperationState state, const QString& error,
                                    bool suppressDialog = false);
    [[nodiscard]] bool validRemoteRequest(const rfm::core::RemoteOperationRequest& request) const;
    [[nodiscard]] bool idInUse(quint64 id) const;
    [[nodiscard]] bool remoteQueueContains(quint64 id) const;
    [[nodiscard]] std::optional<rfm::core::RemoteOperationRequest> takeRemoteOperation(quint64 id);
    [[nodiscard]] rfm::core::RemoteOperationResult
    syntheticResult(const rfm::core::RemoteOperationRequest& request, const QString& error) const;
    [[nodiscard]] bool ownsActive(quint64 id) const;
    [[nodiscard]] bool ownsActiveRemoteOperation(quint64 id) const;

    TransferQueue m_queue;
    std::optional<TransferRequest> m_active;
    QQueue<RemoteOperationRequest> m_remoteQueue;
    std::optional<RemoteOperationRequest> m_activeRemoteOperation;
    QSet<quint64> m_terminalIds;
    QSet<quint64> m_terminalRemoteOperationIds;
    QString m_executorFailure;
    StopMode m_stopMode{StopMode::None};
    bool m_executorAvailable{false};
};

} // namespace rfm::core
