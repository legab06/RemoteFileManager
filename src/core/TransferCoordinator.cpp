#include "remotefilemanager/core/TransferCoordinator.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <algorithm>
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
    dispatchNextRemoteOperation();
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
    if (idInUse(request.id) || !m_queue.enqueue(request)) {
        emit transferRejected(request.id, tr("A transfer with this identifier already exists."));
        return;
    }

    m_terminalIds.remove(request.id);
    publishProgress(progressFor(request, TransferState::Queued));
    dispatchNext();
}

void TransferCoordinator::enqueueRemoteOperation(RemoteOperationRequest request)
{
    if (!m_executorAvailable || m_stopMode != StopMode::None || !validRemoteRequest(request)) {
        emit remoteOperationRejected(request.id,
                                     tr("Invalid remote operation request or no active executor."));
        return;
    }
    if (idInUse(request.id)) {
        emit remoteOperationRejected(request.id,
                                     tr("An operation with this identifier already exists."));
        return;
    }

    m_terminalRemoteOperationIds.remove(request.id);
    m_remoteQueue.enqueue(request);
    publishRemoteProgress(operationProgress(request, OperationState::Queued));
    dispatchNextRemoteOperation();
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

void TransferCoordinator::cancelRemoteOperation(quint64 id)
{
    if (ownsActiveRemoteOperation(id)) {
        emit cancelActiveRemoteOperationRequested(id);
        return;
    }

    const std::optional<RemoteOperationRequest> cancelled = takeRemoteOperation(id);
    if (cancelled.has_value()) {
        terminalizeRemoteOperation(*cancelled, OperationState::Cancelled,
                                   tr("Remote operation cancelled before it started."));
        return;
    }
    emit remoteOperationRejected(id,
                                 tr("The remote operation was not found or is already terminal."));
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

void TransferCoordinator::handleRemoteExecutorProgress(OperationProgress progress)
{
    if (!ownsActiveRemoteOperation(progress.id)) {
        return;
    }
    publishRemoteProgress(progress);
}

void TransferCoordinator::handleRemoteExecutorResult(RemoteOperationResult result)
{
    if (!ownsActiveRemoteOperation(result.id)) {
        return;
    }

    if (!m_terminalRemoteOperationIds.contains(result.id)) {
        publishRemoteProgress(finishRemoteOperation(
            result, operationProgress(*m_activeRemoteOperation, OperationState::Running)));
    }
    if (m_stopMode != StopMode::None) {
        emit remoteOperationResultSilent(result.id);
    }
    emit remoteOperationFinished(result);
    m_activeRemoteOperation.reset();
    if (m_executorFailure.isEmpty() && m_stopMode == StopMode::None) {
        dispatchNextRemoteOperation();
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
    terminalizeQueuedRemoteOperations(OperationState::Failed, m_executorFailure, true);
}

void TransferCoordinator::handleRemoteExecutorFailure(QString error)
{
    handleExecutorFailure(error.isEmpty() ? tr("The remote operation executor became unavailable.")
                                          : std::move(error));
    if (m_activeRemoteOperation.has_value()) {
        emit remoteOperationResultSilent(m_activeRemoteOperation->id);
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
    terminalizeQueuedRemoteOperations(
        OperationState::Cancelled,
        tr("Remote operation cancelled because the application is closing."), true);
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
    terminalizeQueuedRemoteOperations(
        OperationState::Cancelled,
        tr("Remote operation cancelled because the SSH session is disconnecting."), true);
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

void TransferCoordinator::dispatchNextRemoteOperation()
{
    if (m_activeRemoteOperation.has_value() || m_remoteQueue.isEmpty() || !m_executorAvailable ||
        m_stopMode != StopMode::None || !m_executorFailure.isEmpty()) {
        return;
    }
    m_activeRemoteOperation = m_remoteQueue.dequeue();
    emit startRemoteOperationRequested(*m_activeRemoteOperation);
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

void TransferCoordinator::publishRemoteProgress(const OperationProgress& progress)
{
    if (m_terminalRemoteOperationIds.contains(progress.id)) {
        return;
    }
    if (isTerminal(progress.state)) {
        m_terminalRemoteOperationIds.insert(progress.id);
    }
    emit remoteOperationUpdated(progress);
}

void TransferCoordinator::terminalizeQueued(TransferState state, const QString& error)
{
    const QList<TransferRequest> requests = m_queue.takeAll();
    for (const TransferRequest& request : requests) {
        publishProgress(progressFor(request, state, error));
    }
}

void TransferCoordinator::terminalizeQueuedRemoteOperations(OperationState state,
                                                            const QString& error,
                                                            bool suppressDialog)
{
    while (!m_remoteQueue.isEmpty()) {
        terminalizeRemoteOperation(m_remoteQueue.dequeue(), state, error, suppressDialog);
    }
}

void TransferCoordinator::terminalizeRemoteOperation(const RemoteOperationRequest& request,
                                                     OperationState state, const QString& error,
                                                     bool suppressDialog)
{
    if (suppressDialog) {
        emit remoteOperationResultSilent(request.id);
    }
    publishRemoteProgress(operationProgress(request, state, error));
    emit remoteOperationFinished(syntheticResult(request, error));
}

bool TransferCoordinator::validRemoteRequest(const RemoteOperationRequest& request) const
{
    if (request.id == 0 ||
        (request.kind != RemoteOperationKind::Copy && request.kind != RemoteOperationKind::Move) ||
        request.sources.isEmpty() || request.destinationDirectory.isEmpty() ||
        request.destinationDirectory.contains(QChar{'\0'})) {
        return false;
    }
    return std::ranges::all_of(request.sources, [](const RemoteSelection& source) {
        return !source.path.isEmpty() && !source.path.contains(QChar{'\0'});
    });
}

bool TransferCoordinator::idInUse(quint64 id) const
{
    return ownsActive(id) || m_queue.contains(id) || ownsActiveRemoteOperation(id) ||
           remoteQueueContains(id);
}

bool TransferCoordinator::remoteQueueContains(quint64 id) const
{
    return std::ranges::any_of(
        m_remoteQueue, [id](const RemoteOperationRequest& request) { return request.id == id; });
}

std::optional<RemoteOperationRequest> TransferCoordinator::takeRemoteOperation(quint64 id)
{
    for (auto iterator = m_remoteQueue.begin(); iterator != m_remoteQueue.end(); ++iterator) {
        if (iterator->id == id) {
            const RemoteOperationRequest request = *iterator;
            m_remoteQueue.erase(iterator);
            return request;
        }
    }
    return std::nullopt;
}

RemoteOperationResult TransferCoordinator::syntheticResult(const RemoteOperationRequest& request,
                                                           const QString& error) const
{
    RemoteOperationResult result{request.id, request.kind, {}};
    result.items.reserve(request.sources.size());
    for (const RemoteSelection& source : request.sources) {
        const QString normalized = RemotePath::normalize(source.path);
        result.items.push_back(
            {normalized,
             RemotePath::join(request.destinationDirectory, RemotePath::fileName(normalized)),
             false, error});
    }
    return result;
}

bool TransferCoordinator::ownsActive(quint64 id) const
{
    return m_active.has_value() && m_active->id == id;
}

bool TransferCoordinator::ownsActiveRemoteOperation(quint64 id) const
{
    return m_activeRemoteOperation.has_value() && m_activeRemoteOperation->id == id;
}

} // namespace rfm::core
