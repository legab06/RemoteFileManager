#pragma once

#include "remotefilemanager/core/TransferQueue.hpp"

#include <QObject>
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
    void pauseTransfer(quint64 id);
    void resumeTransfer(quint64 id);
    void cancelTransfer(quint64 id);
    void handleExecutorProgress(rfm::core::TransferProgress progress);
    void handleExecutorFailure(QString error);
    void handleExecutorRejection(quint64 id, QString error);
    void shutdownTransfers();
    void disconnectExecutor();
    void handleExecutorShutdown();

  signals:
    void startTransferRequested(rfm::core::TransferRequest request);
    void pauseActiveRequested(quint64 id);
    void resumeActiveRequested(quint64 id);
    void cancelActiveRequested(quint64 id);
    void shutdownExecutorRequested();
    void disconnectExecutorRequested();
    void transferUpdated(rfm::core::TransferProgress progress);
    void transferRejected(quint64 id, QString error);
    void transfersShutdown();

  private:
    enum class StopMode { None, Shutdown, Disconnect };

    void dispatchNext();
    void publishProgress(const rfm::core::TransferProgress& progress);
    void terminalizeQueued(rfm::core::TransferState state, const QString& error);
    [[nodiscard]] bool ownsActive(quint64 id) const;

    TransferQueue m_queue;
    std::optional<TransferRequest> m_active;
    QSet<quint64> m_terminalIds;
    QString m_executorFailure;
    StopMode m_stopMode{StopMode::None};
    bool m_executorAvailable{false};
};

} // namespace rfm::core
