#include "remotefilemanager/core/ServerSideCopyJob.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <utility>

namespace rfm::core
{

ServerSideCopyJob::ServerSideCopyJob(ServerSideCopyBackend& backend, quint64 id,
                                     QList<RemoteSelection> sources,
                                     QString destinationDirectory)
    : m_backend(backend), m_sources(std::move(sources)),
      m_destinationDirectory(RemotePath::normalize(destinationDirectory)),
      m_progress(beginRemoteOperation(id, OperationKind::RemoteCopy, m_sources,
                                      m_destinationDirectory)),
      m_result{id, RemoteOperationKind::Copy, {}}
{
    m_progress.state = OperationState::Queued;
    m_progress.cancellationSupported = true;
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
    case RemoteBackendError::Failure:
        return QStringLiteral("The remote copy failed.");
    case RemoteBackendError::None:
        return {};
    }
    return QStringLiteral("The remote copy failed.");
}

void ServerSideCopyJob::prepareItem()
{
    if (m_sourceIndex >= m_sources.size()) {
        finish();
        return;
    }
    const RemoteSelection& selection = m_sources.at(m_sourceIndex);
    const QString source = RemotePath::normalize(selection.path);
    const QString destination =
        RemotePath::join(m_destinationDirectory, RemotePath::fileName(source));
    RemoteItemResult item{source, destination, false, {}};
    const bool insideSource =
        selection.directory &&
        (m_destinationDirectory == source ||
         m_destinationDirectory.startsWith(source + QChar{'/'}));
    if (RemotePath::isProtected(source) || m_destinationDirectory.isEmpty() ||
        m_destinationDirectory == QStringLiteral("..") ||
        m_destinationDirectory.startsWith(QStringLiteral("../")) || destination.isEmpty() ||
        RemotePath::normalize(destination) == source || insideSource) {
        item.error = insideSource ? QStringLiteral("A folder cannot be copied inside itself.")
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
    m_phase = Phase::StartItem;
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
        m_phase = Phase::Prepare;
    }
}

void ServerSideCopyJob::finish()
{
    m_progress.currentItem.clear();
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
    if (m_phase == Phase::StartItem) {
        RemoteItemResult& item = m_result.items.last();
        const RemoteSelection& selection = m_sources.at(m_sourceIndex);
        const RemoteBackendResult started =
            m_backend.startCopy(item.source, item.destination, selection.directory);
        if (!started.succeeded()) {
            finishItem(started);
            return;
        }
        m_copyActive = true;
        m_phase = Phase::PollItem;
        return;
    }
    if (m_phase == Phase::PollItem) {
        const std::optional<RemoteBackendResult> polled = m_backend.pollCopy();
        if (polled.has_value()) {
            finishItem(*polled);
        }
        return;
    }
    if (m_phase == Phase::RequestCancellation) {
        const std::optional<RemoteBackendResult> requested =
            m_backend.requestCopyCancellation();
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
        const std::optional<RemoteBackendResult> cancellation =
            m_backend.pollCopyCancellation();
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
            {source,
             RemotePath::join(m_destinationDirectory, RemotePath::fileName(source)), false,
             QStringLiteral("Remote copy cancelled.")});
    } else if (!m_result.items.isEmpty() && !m_result.items.last().success &&
               m_result.items.last().error.isEmpty()) {
        m_result.items.last().error = QStringLiteral("Remote copy cancelled.");
    }
    for (qsizetype index = m_sourceIndex + 1; index < m_sources.size(); ++index) {
        const QString source = RemotePath::normalize(m_sources.at(index).path);
        m_result.items.push_back(
            {source,
             RemotePath::join(m_destinationDirectory, RemotePath::fileName(source)), false,
             QStringLiteral("Remote copy cancelled.")});
    }
}

bool ServerSideCopyJob::requestCancel()
{
    if (isFinished() || m_progress.state == OperationState::Cancelling) {
        return false;
    }
    if (m_sourceIndex == m_sources.size() && !m_copyActive) {
        finish();
        return false;
    }
    m_progress.state = OperationState::Cancelling;
    if (m_copyActive) {
        m_phase = Phase::RequestCancellation;
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

const OperationProgress& ServerSideCopyJob::progress() const { return m_progress; }

const RemoteOperationResult& ServerSideCopyJob::result() const { return m_result; }

} // namespace rfm::core
