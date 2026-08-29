#include "remotefilemanager/core/ServerSideCopyJob.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QUuid>

#include <utility>

namespace rfm::core
{

ServerSideCopyJob::ServerSideCopyJob(ServerSideCopyBackend& backend, quint64 id,
                                     QList<RemoteSelection> sources, QString destinationDirectory,
                                     RemoteOperationKind operationKind)
    : m_backend(backend), m_sources(std::move(sources)),
      m_destinationDirectory(RemotePath::normalize(destinationDirectory)),
      m_progress(beginRemoteOperation(id,
                                      operationKind == RemoteOperationKind::Move
                                          ? OperationKind::RemoteMove
                                          : OperationKind::RemoteCopy,
                                      m_sources, m_destinationDirectory)),
      m_result{id, operationKind, {}}, m_operationKind(operationKind)
{
    m_progress.state = OperationState::Preparing;
    m_progress.cancellationSupported = operationKind == RemoteOperationKind::Copy;
}

QString ServerSideCopyJob::describeError(const RemoteBackendResult& result) const
{
    if (!result.detail.isEmpty()) {
        return result.detail;
    }
    switch (result.error) {
    case RemoteBackendError::NotFound:
        return QStringLiteral("The remote item was not found.");
    case RemoteBackendError::AlreadyExists:
        return QStringLiteral("A remote item already exists at this destination.");
    case RemoteBackendError::PermissionDenied:
        return QStringLiteral("Permission denied by the server.");
    case RemoteBackendError::Unsupported:
        return QStringLiteral("Server-side copy is not supported by the server.");
    case RemoteBackendError::InvalidPath:
        return QStringLiteral("Invalid or protected remote path.");
    case RemoteBackendError::CrossDevice:
        return QStringLiteral("The source and destination are on different filesystems.");
    case RemoteBackendError::Failure:
        return QStringLiteral("The remote copy failed.");
    case RemoteBackendError::None:
        return {};
    }
    return QStringLiteral("The remote copy failed.");
}

QString ServerSideCopyJob::stagingDirectory(const QString& destination) const
{
    const QString operation = m_operationKind == RemoteOperationKind::Move ? QStringLiteral("move")
                                                                           : QStringLiteral("copy");
    const QString temporaryName =
        QStringLiteral(".rfm-%1-%2.partial")
            .arg(operation, QUuid::createUuid().toString(QUuid::WithoutBraces));
    return RemotePath::join(RemotePath::parent(destination), temporaryName);
}

void ServerSideCopyJob::failTransport(QString error)
{
    if (isFinished()) {
        return;
    }
    if (error.isEmpty()) {
        error = QStringLiteral("The SSH/SFTP transport was lost during the remote operation.");
    }

    if (m_sourceIndex < m_sources.size()) {
        const QString source = RemotePath::normalize(m_sources.at(m_sourceIndex).path);
        const QString destination =
            RemotePath::join(m_destinationDirectory, RemotePath::fileName(source));
        if (m_result.items.size() == m_sourceIndex) {
            m_result.items.push_back({source, destination, false, {}});
        }
        RemoteItemResult& current = m_result.items[m_sourceIndex];
        if (!current.success) {
            current.error = error;
            if (m_stagingOwned && !m_stagingDirectory.isEmpty()) {
                current.error +=
                    QStringLiteral(" Temporary %1 staging may remain at %2; its contents are "
                                   "indeterminate because cleanup was not attempted after the "
                                   "transport loss.")
                        .arg(m_operationKind == RemoteOperationKind::Move ? QStringLiteral("move")
                                                                          : QStringLiteral("copy"),
                             m_stagingDirectory);
            }
        }
        for (qsizetype index = m_sourceIndex + 1; index < m_sources.size(); ++index) {
            const QString remainingSource = RemotePath::normalize(m_sources.at(index).path);
            m_result.items.push_back(
                {remainingSource,
                 RemotePath::join(m_destinationDirectory, RemotePath::fileName(remainingSource)),
                 false, QStringLiteral("%1 The item was not started.").arg(error)});
        }
    }

    m_copyActive = false;
    m_progress.currentItem.clear();
    m_progress.cancellationSupported = false;
    m_progress.state = OperationState::Failed;
    QStringList failures;
    for (const RemoteItemResult& item : std::as_const(m_result.items)) {
        if (!item.success) {
            failures.push_back(QStringLiteral("%1: %2").arg(item.source, item.error));
        }
    }
    m_progress.error = failures.join(QChar{'\n'});
    m_phase = Phase::Finished;
}

void ServerSideCopyJob::prepareItem()
{
    if (m_sourceIndex >= m_sources.size()) {
        finish();
        return;
    }
    const RemoteSelection& selection = m_sources.at(m_sourceIndex);
    m_stagingDirectory.clear();
    m_pendingResult = {};
    m_cleanupContinuation = CleanupContinuation::None;
    m_stagingOwned = false;
    const QString source = RemotePath::normalize(selection.path);
    const QString destination =
        RemotePath::join(m_destinationDirectory, RemotePath::fileName(source));
    RemoteItemResult item{source, destination, false, {}};
    const bool insideSource =
        selection.directory && (m_destinationDirectory == source ||
                                m_destinationDirectory.startsWith(source + QChar{'/'}));
    if (RemotePath::isProtected(source) || m_destinationDirectory.isEmpty() ||
        m_destinationDirectory == QStringLiteral("..") ||
        m_destinationDirectory.startsWith(QStringLiteral("../")) || destination.isEmpty() ||
        RemotePath::normalize(destination) == source || insideSource) {
        item.error = insideSource
                         ? QStringLiteral("A folder cannot be copied or moved inside itself.")
                         : QStringLiteral("Invalid or protected remote path.");
        m_result.items.push_back(item);
        ++m_sourceIndex;
        return;
    }

    const RemoteProbeResult existing = m_backend.probe(destination);
    if (existing.result.succeeded() && existing.node.exists) {
        item.error = QStringLiteral("A remote item already exists at this destination.");
        m_result.items.push_back(item);
        ++m_sourceIndex;
        return;
    }
    if (existing.result.error != RemoteBackendError::NotFound) {
        item.error = describeError(existing.result);
        m_result.items.push_back(item);
        ++m_sourceIndex;
        return;
    }

    m_result.items.push_back(item);
    m_progress.state = OperationState::Running;
    m_progress.currentItem = source;
    m_phase =
        m_operationKind == RemoteOperationKind::Move ? Phase::RenameItem : Phase::PrepareStaging;
}

void ServerSideCopyJob::finishItem(const RemoteBackendResult& result)
{
    RemoteItemResult& item = m_result.items.last();
    item.success = result.succeeded();
    item.error = describeError(result);
    if (item.success) {
        ++m_progress.completedItems;
    }
    m_copyActive = false;
    ++m_sourceIndex;
    if (m_sourceIndex == m_sources.size()) {
        finish();
    } else {
        m_progress.cancellationSupported = m_operationKind == RemoteOperationKind::Copy;
        m_phase = Phase::Prepare;
    }
}

void ServerSideCopyJob::finishRemoval(const RemoteBackendResult& result)
{
    if (result.succeeded()) {
        finishItem(result);
        return;
    }
    RemoteBackendResult contextual = result;
    const QString detail = describeError(result);
    contextual.detail =
        QStringLiteral("The item was copied, but its source could not be removed. %1").arg(detail);
    finishItem(contextual);
}

void ServerSideCopyJob::beginPostPromotionCleanup()
{
    m_pendingResult = {
        RemoteBackendError::None,
        QStringLiteral("The remote copy was promoted, but cleanup did not complete.")};
    m_cleanupContinuation = CleanupContinuation::FinishSuccess;
    m_progress.cancellationSupported = false;
    m_copyActive = false;
    m_phase = Phase::RemoveEmptyStaging;
}

void ServerSideCopyJob::beginCleanup(CleanupContinuation continuation,
                                     const RemoteBackendResult& result)
{
    m_pendingResult = result;
    m_cleanupContinuation = continuation;
    m_progress.cancellationSupported = false;
    m_copyActive = false;
    if (!m_stagingOwned) {
        finishCleanup({});
        return;
    }
    m_phase = Phase::StartCleanup;
}

RemoteBackendResult
ServerSideCopyJob::cleanupFailure(const RemoteBackendResult& cleanupResult) const
{
    const QString cleanupDetail = describeError(cleanupResult);
    RemoteBackendResult combined = m_pendingResult;
    combined.error = RemoteBackendError::Failure;
    const QString original = describeError(m_pendingResult);
    const QString operation = m_operationKind == RemoteOperationKind::Move ? QStringLiteral("move")
                                                                           : QStringLiteral("copy");
    combined.detail =
        QStringLiteral("%1 Temporary %2 data remains at %3 because cleanup failed: %4")
            .arg(original.isEmpty()
                     ? QStringLiteral("The remote %1 did not complete.").arg(operation)
                     : original,
                 operation, m_stagingDirectory, cleanupDetail);
    return combined;
}

void ServerSideCopyJob::finishCleanup(const RemoteBackendResult& cleanupResult)
{
    if (cleanupResult.succeeded()) {
        m_stagingOwned = false;
    }

    const CleanupContinuation continuation = m_cleanupContinuation;
    m_cleanupContinuation = CleanupContinuation::None;
    if (continuation == CleanupContinuation::RemoveSource) {
        if (cleanupResult.succeeded()) {
            m_phase = Phase::StartRemove;
        } else {
            RemoteBackendResult failure{
                RemoteBackendError::Failure,
                QStringLiteral("The destination was created, but temporary move "
                               "cleanup failed. The source was preserved. Temporary "
                               "data remains at %1: %2")
                    .arg(m_stagingDirectory, describeError(cleanupResult))};
            finishItem(failure);
        }
        return;
    }
    if (continuation == CleanupContinuation::FinishFailure) {
        finishItem(cleanupResult.succeeded() ? m_pendingResult : cleanupFailure(cleanupResult));
        return;
    }
    if (continuation == CleanupContinuation::FinishSuccess) {
        finishItem(cleanupResult.succeeded() ? RemoteBackendResult{}
                                             : cleanupFailure(cleanupResult));
        return;
    }
    if (continuation == CleanupContinuation::FinishCancellation) {
        appendCancelledItems();
        m_progress.currentItem.clear();
        if (!cleanupResult.succeeded()) {
            const RemoteBackendResult failure = cleanupFailure(cleanupResult);
            m_result.items.last().error = failure.detail;
            m_progress.state = OperationState::Failed;
            m_progress.error = failure.detail;
        } else if (m_pendingResult.succeeded()) {
            m_progress.state = OperationState::Cancelled;
            m_progress.error = QStringLiteral("Remote copy cancelled.");
        } else {
            const QString failure = describeError(m_pendingResult);
            m_result.items.last().error =
                QStringLiteral("Remote %1 cancellation failed. %2")
                    .arg(m_operationKind == RemoteOperationKind::Move ? QStringLiteral("move")
                                                                      : QStringLiteral("copy"),
                         failure);
            m_progress.state = OperationState::Failed;
            m_progress.error = failure;
        }
        m_phase = Phase::Finished;
    }
}

void ServerSideCopyJob::finish()
{
    m_progress.currentItem.clear();
    m_progress.cancellationSupported = false;
    m_progress.state = m_result.allSucceeded() ? OperationState::Completed : OperationState::Failed;
    m_progress.error.clear();
    QStringList errors;
    for (const RemoteItemResult& item : std::as_const(m_result.items)) {
        if (!item.success) {
            errors.push_back(QStringLiteral("%1: %2").arg(item.source, item.error));
        }
    }
    m_progress.error = errors.join(QChar{'\n'});
    m_phase = Phase::Finished;
}

void ServerSideCopyJob::step()
{
    if (isFinished()) {
        return;
    }
    if (m_phase == Phase::Prepare) {
        m_progress.state = OperationState::Preparing;
        prepareItem();
        return;
    }
    if (m_phase == Phase::RenameItem) {
        RemoteItemResult& item = m_result.items.last();
        const RemoteBackendResult renamed = m_backend.rename(item.source, item.destination);
        if (renamed.succeeded()) {
            finishItem(renamed);
        } else if (renamed.error == RemoteBackendError::CrossDevice) {
            m_progress.cancellationSupported = true;
            m_phase = Phase::PrepareStaging;
        } else {
            finishItem(renamed);
        }
        return;
    }
    if (m_phase == Phase::PrepareStaging) {
        const RemoteItemResult& item = m_result.items.last();
        m_stagingDirectory = stagingDirectory(item.destination);
        const RemoteBackendResult reserved = m_backend.reserveStaging(m_stagingDirectory);
        if (!reserved.succeeded()) {
            finishItem(
                reserved.error == RemoteBackendError::AlreadyExists
                    ? RemoteBackendResult{RemoteBackendError::AlreadyExists,
                                          QStringLiteral(
                                              "A temporary remote staging item already exists.")}
                    : reserved);
        } else {
            m_stagingOwned = true;
            m_phase = Phase::StartItem;
        }
        return;
    }
    if (m_phase == Phase::StartItem) {
        RemoteItemResult& item = m_result.items.last();
        const RemoteSelection& selection = m_sources.at(m_sourceIndex);
        const RemoteBackendResult started =
            m_operationKind == RemoteOperationKind::Move
                ? m_backend.startMoveStagingCopy(
                      item.source, RemotePath::join(m_stagingDirectory, QStringLiteral("item")))
                : m_backend.startCopy(item.source,
                                      RemotePath::join(m_stagingDirectory, QStringLiteral("item")),
                                      selection.directory);
        if (!started.succeeded()) {
            beginCleanup(CleanupContinuation::FinishFailure, started);
            return;
        }
        m_copyActive = true;
        m_phase = Phase::PollItem;
        return;
    }
    if (m_phase == Phase::PollItem) {
        const std::optional<RemoteBackendResult> polled = m_backend.pollCopy();
        if (polled.has_value()) {
            if (polled->succeeded()) {
                m_progress.state = OperationState::Finalizing;
                m_progress.cancellationSupported = false;
                m_copyActive = false;
                m_phase = Phase::PromoteItem;
            } else {
                beginCleanup(CleanupContinuation::FinishFailure, *polled);
            }
        }
        return;
    }
    if (m_phase == Phase::PromoteItem) {
        const RemoteItemResult& item = m_result.items.last();
        const RemoteProbeResult existing = m_backend.probe(item.destination);
        if (existing.result.succeeded() && existing.node.exists) {
            beginCleanup(
                CleanupContinuation::FinishFailure,
                {RemoteBackendError::AlreadyExists,
                 QStringLiteral("A remote item appeared at the destination before promotion.")});
            return;
        }
        if (existing.result.error != RemoteBackendError::NotFound) {
            beginCleanup(CleanupContinuation::FinishFailure, existing.result);
            return;
        }
        const RemoteBackendResult promoted = m_backend.rename(
            RemotePath::join(m_stagingDirectory, QStringLiteral("item")), item.destination);
        if (!promoted.succeeded()) {
            RemoteBackendResult contextual = promoted;
            contextual.detail =
                QStringLiteral("The item was copied to a temporary destination, but could "
                               "not be promoted. The source was preserved. %1")
                    .arg(describeError(promoted));
            beginCleanup(CleanupContinuation::FinishFailure, contextual);
        } else {
            if (m_operationKind == RemoteOperationKind::Move) {
                beginCleanup(CleanupContinuation::RemoveSource, {});
            } else {
                beginPostPromotionCleanup();
            }
        }
        return;
    }
    if (m_phase == Phase::RemoveEmptyStaging) {
        finishCleanup(m_backend.removeEmptyDirectory(m_stagingDirectory));
        return;
    }
    if (m_phase == Phase::StartCleanup) {
        const RemoteBackendResult started = m_backend.startRemove(m_stagingDirectory, true, true);
        if (!started.succeeded()) {
            finishCleanup(started);
        } else {
            m_copyActive = true;
            m_phase = Phase::PollCleanup;
        }
        return;
    }
    if (m_phase == Phase::PollCleanup) {
        const std::optional<RemoteBackendResult> polled = m_backend.pollRemove();
        if (polled.has_value()) {
            m_copyActive = false;
            finishCleanup(*polled);
        }
        return;
    }
    if (m_phase == Phase::StartRemove) {
        const RemoteItemResult& item = m_result.items.last();
        const RemoteSelection& selection = m_sources.at(m_sourceIndex);
        const RemoteBackendResult started =
            m_backend.startRemove(item.source, selection.directory, selection.directory);
        if (!started.succeeded()) {
            finishRemoval(started);
            return;
        }
        m_copyActive = true;
        m_phase = Phase::PollRemove;
        return;
    }
    if (m_phase == Phase::PollRemove) {
        const std::optional<RemoteBackendResult> polled = m_backend.pollRemove();
        if (polled.has_value()) {
            finishRemoval(*polled);
        }
        return;
    }
    if (m_phase == Phase::RequestCancellation) {
        const std::optional<RemoteBackendResult> requested = m_backend.requestCopyCancellation();
        if (requested.has_value()) {
            if (requested->succeeded()) {
                m_phase = Phase::PollCancellation;
            } else {
                finishCancellation(*requested);
            }
        }
        return;
    }
    if (m_phase == Phase::PollCancellation) {
        const std::optional<RemoteBackendResult> cancellation = m_backend.pollCopyCancellation();
        if (cancellation.has_value()) {
            finishCancellation(*cancellation);
        }
    }
}

void ServerSideCopyJob::appendCancelledItems()
{
    if (m_result.items.size() == m_sourceIndex && m_sourceIndex < m_sources.size()) {
        const QString source = RemotePath::normalize(m_sources.at(m_sourceIndex).path);
        m_result.items.push_back(
            {source, RemotePath::join(m_destinationDirectory, RemotePath::fileName(source)), false,
             QStringLiteral("Remote copy cancelled.")});
    } else if (!m_result.items.isEmpty() && !m_result.items.last().success &&
               m_result.items.last().error.isEmpty()) {
        m_result.items.last().error = QStringLiteral("Remote copy cancelled.");
    }
    for (qsizetype index = m_sourceIndex + 1; index < m_sources.size(); ++index) {
        const QString source = RemotePath::normalize(m_sources.at(index).path);
        m_result.items.push_back(
            {source, RemotePath::join(m_destinationDirectory, RemotePath::fileName(source)), false,
             QStringLiteral("Remote copy cancelled.")});
    }
}

bool ServerSideCopyJob::requestCancel()
{
    if (isFinished() || !m_progress.cancellationSupported ||
        m_progress.state == OperationState::Cancelling) {
        return false;
    }
    if (m_sourceIndex == m_sources.size() && !m_copyActive) {
        finish();
        return false;
    }
    if (m_phase == Phase::RemoveEmptyStaging || m_phase == Phase::StartCleanup ||
        m_phase == Phase::PollCleanup || m_phase == Phase::StartRemove ||
        m_phase == Phase::PollRemove) {
        return false;
    }
    m_progress.state = OperationState::Cancelling;
    if (m_copyActive) {
        m_phase = Phase::RequestCancellation;
        return true;
    }
    if (m_stagingOwned) {
        beginCleanup(CleanupContinuation::FinishCancellation, {});
        return true;
    }
    appendCancelledItems();
    m_progress.currentItem.clear();
    m_progress.state = OperationState::Cancelled;
    m_progress.error = QStringLiteral("Remote copy cancelled.");
    m_phase = Phase::Finished;
    return true;
}

void ServerSideCopyJob::finishCancellation(const RemoteBackendResult& result)
{
    m_copyActive = false;
    if (m_stagingOwned && result.succeeded()) {
        beginCleanup(CleanupContinuation::FinishCancellation, result);
        return;
    }
    if (m_stagingOwned && m_operationKind == RemoteOperationKind::Copy) {
        appendCancelledItems();
        const QString failure = describeError(result);
        const QString detail =
            QStringLiteral("Remote copy cancellation failed. Temporary copy data remains at %1 "
                           "because process termination could not be confirmed: %2")
                .arg(m_stagingDirectory, failure);
        m_result.items.last().error = detail;
        m_progress.currentItem.clear();
        m_progress.cancellationSupported = false;
        m_progress.state = OperationState::Failed;
        m_progress.error = detail;
        m_phase = Phase::Finished;
        return;
    }
    if (m_stagingOwned) {
        beginCleanup(CleanupContinuation::FinishCancellation, result);
        return;
    }
    appendCancelledItems();
    m_progress.currentItem.clear();
    if (result.succeeded()) {
        m_progress.state = OperationState::Cancelled;
        m_progress.error = QStringLiteral("Remote copy cancelled.");
    } else {
        m_progress.state = OperationState::Failed;
        m_progress.error = describeError(result);
        if (!m_result.items.isEmpty()) {
            m_result.items.last().error =
                QStringLiteral("Remote copy cancellation failed. %1").arg(m_progress.error);
        }
    }
    m_phase = Phase::Finished;
}

bool ServerSideCopyJob::isFinished() const { return m_phase == Phase::Finished; }

std::optional<QString> ServerSideCopyJob::ownedStagingPath() const
{
    return !isFinished() && m_stagingOwned && !m_stagingDirectory.isEmpty()
               ? std::optional<QString>{m_stagingDirectory}
               : std::nullopt;
}

bool ServerSideCopyJob::ownsInternalPath(const QString& path) const
{
    const std::optional<QString> staging = ownedStagingPath();
    if (!staging.has_value()) {
        return false;
    }
    const QString normalized = RemotePath::normalize(path);
    return normalized == *staging || normalized.startsWith(*staging + QChar{'/'});
}

bool ServerSideCopyJob::hidesListingEntry(const QString& parentPath, const QString& entryName) const
{
    const std::optional<QString> staging = ownedStagingPath();
    return staging.has_value() && RemotePath::join(parentPath, entryName) == *staging;
}

const OperationProgress& ServerSideCopyJob::progress() const { return m_progress; }

const RemoteOperationResult& ServerSideCopyJob::result() const { return m_result; }

} // namespace rfm::core
