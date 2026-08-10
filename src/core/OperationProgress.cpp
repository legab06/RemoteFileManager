#include "remotefilemanager/core/OperationProgress.hpp"

#include <algorithm>

namespace rfm::core
{
namespace
{

OperationState operationState(TransferState state)
{
    switch (state) {
    case TransferState::Queued:
        return OperationState::Queued;
    case TransferState::Preparing:
        return OperationState::Preparing;
    case TransferState::Transferring:
        return OperationState::Running;
    case TransferState::Paused:
        return OperationState::Paused;
    case TransferState::Finalizing:
        return OperationState::Finalizing;
    case TransferState::Cancelling:
        return OperationState::Cancelling;
    case TransferState::Completed:
        return OperationState::Completed;
    case TransferState::Cancelled:
        return OperationState::Cancelled;
    case TransferState::Failed:
        return OperationState::Failed;
    }
    return OperationState::Failed;
}

OperationKind operationKind(RemoteOperationKind kind)
{
    return kind == RemoteOperationKind::Move ? OperationKind::RemoteMove
                                             : OperationKind::RemoteCopy;
}

} // namespace

bool isTerminal(OperationState state)
{
    return state == OperationState::Completed || state == OperationState::Cancelled ||
           state == OperationState::Failed;
}

OperationProgress operationProgress(const TransferProgress& transfer)
{
    return {transfer.id,
            transfer.direction == TransferDirection::Upload ? OperationKind::Upload
                                                            : OperationKind::Download,
            operationState(transfer.state),
            {transfer.source},
            transfer.destination,
            transfer.transferredBytes,
            transfer.totalBytes,
            transfer.bytesPerSecond,
            transfer.completedFiles,
            transfer.totalFiles,
            transfer.currentItem,
            transfer.error,
            true,
            true,
            true,
            {}};
}

OperationProgress beginRemoteOperation(quint64 id, OperationKind kind,
                                        const QList<RemoteSelection>& sources,
                                        const QString& destinationDirectory)
{
    OperationProgress operation;
    operation.id = id;
    operation.kind = kind;
    operation.state = OperationState::Running;
    operation.destination = destinationDirectory;
    operation.totalItems = static_cast<quint64>(sources.size());
    operation.sources.reserve(sources.size());
    for (const RemoteSelection& source : sources) {
        operation.sources.push_back(source.path);
    }
    return operation;
}

OperationProgress finishRemoteOperation(const RemoteOperationResult& result,
                                         const OperationProgress& started)
{
    OperationProgress operation = started;
    operation.id = result.id;
    operation.kind = operationKind(result.kind);
    operation.state = result.allSucceeded() ? OperationState::Completed : OperationState::Failed;
    operation.completedItems = static_cast<quint64>(
        std::ranges::count(result.items, true, &RemoteItemResult::success));
    operation.totalItems = static_cast<quint64>(result.items.size());
    operation.error.clear();

    if (operation.sources.isEmpty()) {
        for (const RemoteItemResult& item : result.items) {
            operation.sources.push_back(item.source);
        }
    }
    if (operation.destination.isEmpty() && !result.items.isEmpty()) {
        operation.destination = result.items.constFirst().destination;
    }
    QStringList failures;
    for (const RemoteItemResult& item : result.items) {
        if (!item.success) {
            const QString label = !item.source.isEmpty() && !item.destination.isEmpty()
                                      ? QStringLiteral("%1 → %2").arg(item.source, item.destination)
                                      : (item.source.isEmpty() ? item.destination : item.source);
            failures.push_back(QStringLiteral("%1: %2").arg(label, item.error));
        }
    }
    operation.error = failures.join(QChar{'\n'});
    return operation;
}

} // namespace rfm::core
