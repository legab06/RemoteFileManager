#include "remotefilemanager/core/TransferCoordinator.hpp"

#include <utility>

namespace rfm::core
{
namespace
{

bool isTerminal(TransferState state)
{
    return state == TransferState::Completed || state == TransferState::Cancelled ||
           state == TransferState::Failed;
}

TransferProgress progressFor(const TransferRequest& request, TransferState state,
                             const QString& error = {})
{
    return {request.id, state, request.source,    request.destination, 0, 0, 0, error, 0,
            0,          {},    request.direction, request.directory};
}

} // namespace

TransferCoordinator::TransferCoordinator(QObject* parent) : QObject(parent) {}

void TransferCoordinator::executorConnected()
{
    m_executorAvailable = true;
    m_executorFailure.clear();
    m_stopMode = StopMode::None;
    dispatchNext();
}

void TransferCoordinator::executorDisconnected()
{
    m_executorAvailable = false;
    m_stopMode = StopMode::None;
    m_executorFailure.clear();
}

void TransferCoordinator::enqueueTransfer(TransferRequest request)
{
    if (!m_executorAvailable || m_stopMode != StopMode::None || request.id == 0 ||
        request.source.isEmpty() || request.destination.isEmpty() ||
        request.source.contains(QChar{'\0'}) || request.destination.contains(QChar{'\0'})) {
        emit transferRejected(request.id,
                              tr("Invalid transfer request or no active transfer executor."));
        return;
    }
    if (ownsActive(request.id) || !m_queue.enqueue(request)) {
        emit transferRejected(request.id, tr("A transfer with this identifier already exists."));
        return;
    }

    m_terminalIds.remove(request.id);
    publishProgress(progressFor(request, TransferState::Queued));
    dispatchNext();
}

void TransferCoordinator::pauseTransfer(quint64 id)
{
    if (ownsActive(id)) {
        emit pauseActiveRequested(id);
        return;
    }
    emit transferRejected(id, tr("Only the active transfer can be paused."));
}

void TransferCoordinator::resumeTransfer(quint64 id)
{
    if (ownsActive(id)) {
        emit resumeActiveRequested(id);
        return;
    }
    emit transferRejected(id, tr("Only the active transfer can be resumed."));
}

void TransferCoordinator::cancelTransfer(quint64 id)
{
    if (ownsActive(id)) {
        emit cancelActiveRequested(id);
        return;
    }

    TransferRequest cancelled;
    if (m_queue.cancel(id, cancelled)) {
        publishProgress(progressFor(cancelled, TransferState::Cancelled));
        return;
    }
    emit transferRejected(id, tr("The transfer was not found or is already terminal."));
}

void TransferCoordinator::handleExecutorProgress(TransferProgress progress)
{
    if (!ownsActive(progress.id)) {
        return;
    }

    publishProgress(progress);
    if (!isTerminal(progress.state)) {
        return;
    }

    m_active.reset();
    if (!m_executorFailure.isEmpty()) {
        terminalizeQueued(TransferState::Failed, m_executorFailure);
        return;
    }
    if (m_stopMode == StopMode::None) {
        dispatchNext();
    }
}

void TransferCoordinator::handleExecutorFailure(QString error)
{
    m_executorAvailable = false;
    m_executorFailure =
        error.isEmpty() ? tr("The transfer executor became unavailable.") : std::move(error);
    if (!m_active.has_value()) {
        terminalizeQueued(TransferState::Failed, m_executorFailure);
    }
}

void TransferCoordinator::handleExecutorRejection(quint64 id, QString error)
{
    emit transferRejected(id, std::move(error));
}

void TransferCoordinator::shutdownTransfers()
{
    if (m_stopMode == StopMode::Shutdown) {
        return;
    }
    m_stopMode = StopMode::Shutdown;
    m_executorAvailable = false;
    terminalizeQueued(TransferState::Cancelled,
                      tr("Transfer cancelled because the application is closing."));
    emit shutdownExecutorRequested();
}

void TransferCoordinator::disconnectExecutor()
{
    if (m_stopMode == StopMode::Disconnect) {
        return;
    }
    m_stopMode = StopMode::Disconnect;
    m_executorAvailable = false;
    terminalizeQueued(TransferState::Cancelled,
                      tr("Transfer cancelled because the SSH session is disconnecting."));
    emit disconnectExecutorRequested();
}

void TransferCoordinator::handleExecutorShutdown() { emit transfersShutdown(); }

void TransferCoordinator::dispatchNext()
{
    if (m_active.has_value() || !m_executorAvailable || m_stopMode != StopMode::None ||
        !m_executorFailure.isEmpty()) {
        return;
    }
    m_active = m_queue.takeNext();
    if (m_active.has_value()) {
        emit startTransferRequested(*m_active);
    }
}

void TransferCoordinator::publishProgress(const TransferProgress& progress)
{
    if (isTerminal(progress.state)) {
        if (m_terminalIds.contains(progress.id)) {
            return;
        }
        m_terminalIds.insert(progress.id);
    }
    emit transferUpdated(progress);
}

void TransferCoordinator::terminalizeQueued(TransferState state, const QString& error)
{
    const QList<TransferRequest> requests = m_queue.takeAll();
    for (const TransferRequest& request : requests) {
        publishProgress(progressFor(request, state, error));
    }
}

bool TransferCoordinator::ownsActive(quint64 id) const
{
    return m_active.has_value() && m_active->id == id;
}

} // namespace rfm::core
