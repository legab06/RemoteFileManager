#include "remotefilemanager/core/TransferDirectoryJob.hpp"

#include "remotefilemanager/core/LocalDownloadPath.hpp"
#include "remotefilemanager/core/RemotePath.hpp"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <optional>
#include <utility>

namespace rfm::core
{
namespace
{

QString remotePathForRelative(const QString& root, const QString& relative)
{
    QString result = root;
    const QString portable = QDir::fromNativeSeparators(relative);
    for (const QString& component : portable.split(QChar{'/'}, Qt::SkipEmptyParts)) {
        result = RemotePath::join(result, component);
        if (result.isEmpty()) {
            return {};
        }
    }
    return result;
}

bool remoteMissing(const TransferStatResult& result)
{
    return (!result.result.succeeded() && result.result.error == TransferBackendError::NotFound) ||
           (result.result.succeeded() && !result.node.exists);
}

QString backendErrorText(const TransferBackendResult& result)
{
    if (!result.detail.isEmpty()) {
        return result.detail;
    }
    switch (result.error) {
    case TransferBackendError::NotFound:
        return QStringLiteral("The remote path was not found.");
    case TransferBackendError::AlreadyExists:
        return QStringLiteral("The remote path already exists.");
    case TransferBackendError::PermissionDenied:
        return QStringLiteral("Permission was denied by the server.");
    case TransferBackendError::ConnectionLost:
        return QStringLiteral("The SSH/SFTP connection was lost.");
    case TransferBackendError::Unsupported:
        return QStringLiteral("The server does not support this operation.");
    case TransferBackendError::Io:
        return QStringLiteral("The server reported an I/O error.");
    case TransferBackendError::Failure:
        return QStringLiteral("The remote operation failed.");
    case TransferBackendError::None:
        return {};
    }
    return QStringLiteral("The remote operation failed.");
}

QString backendFailure(const QString& operation, const QString& path,
                       const TransferBackendResult& result)
{
    return QStringLiteral("%1 %2: %3").arg(operation, path, backendErrorText(result));
}

} // namespace

TransferDirectoryJob::TransferDirectoryJob(RemoteTransferBackend& backend, TransferRequest request,
                                           LocalPathFlavor localPathFlavor)
    : m_backend(backend), m_request(std::move(request)), m_progress{m_request.id,
                                                                    TransferState::Queued,
                                                                    m_request.source,
                                                                    m_request.destination,
                                                                    0,
                                                                    0,
                                                                    0,
                                                                    {},
                                                                    0,
                                                                    0,
                                                                    {},
                                                                    m_request.direction,
                                                                    true},
      m_localPathFlavor(localPathFlavor)
{}

void TransferDirectoryJob::appendCleanupError(const QString& error)
{
    if (error.isEmpty()) {
        return;
    }
    if (!m_progress.error.isEmpty()) {
        m_progress.error += QChar{' '};
    }
    m_progress.error += error;
}

void TransferDirectoryJob::fail(const QString& error, const QString& item)
{
    m_progress.currentItem = item;
    m_progress.error = item.isEmpty() ? error : QStringLiteral("%1: %2").arg(item, error);
    m_child.reset();
    if (m_remoteDirectoryHandle != 0) {
        const TransferBackendResult result = m_backend.closeDirectory(m_remoteDirectoryHandle);
        if (!result.succeeded()) {
            appendCleanupError(QStringLiteral("The remote directory handle could not be closed."));
        }
        m_remoteDirectoryHandle = 0;
    }
    m_progress.state = TransferState::Failed;
    m_phase = Phase::Finished;
}

void TransferDirectoryJob::prepare()
{
    m_progress.state = TransferState::Preparing;
    if (m_request.direction == TransferDirection::Upload) {
        const QFileInfo source(m_request.source);
        if (!source.exists() || !source.isDir() || source.isSymbolicLink()) {
            fail(QStringLiteral("The local directory is missing, invalid, or symbolic."),
                 m_request.source);
            return;
        }
        const QString destination = RemotePath::normalize(m_request.destination);
        if (destination.isEmpty() || RemotePath::isProtected(destination)) {
            fail(QStringLiteral("The remote destination directory is invalid."),
                 m_request.destination);
            return;
        }
        m_request.destination = destination;
        m_progress.destination = destination;
        const TransferStatResult target = m_backend.stat(destination);
        if (!remoteMissing(target)) {
            fail(target.node.exists
                     ? QStringLiteral("The destination directory already exists.")
                     : backendFailure(QStringLiteral("Unable to inspect remote destination"),
                                      destination, target.result),
                 destination);
            return;
        }
        m_localIterator = std::make_unique<QDirIterator>(source.absoluteFilePath(),
                                                         QDir::AllEntries | QDir::Hidden |
                                                             QDir::System | QDir::NoDotAndDotDot,
                                                         QDirIterator::Subdirectories);
        m_phase = Phase::DiscoverUpload;
        return;
    }

    const TransferStatResult source = m_backend.stat(m_request.source);
    if (!source.result.succeeded()) {
        fail(backendFailure(QStringLiteral("Unable to inspect remote source"), m_request.source,
                            source.result),
             m_request.source);
        return;
    }
    if (!source.node.exists) {
        fail(QStringLiteral("The remote directory was not found."), m_request.source);
        return;
    }
    if (!source.node.isDirectory() || source.node.isSymbolicLink()) {
        fail(QStringLiteral("The remote source is not a transferable directory."),
             m_request.source);
        return;
    }
    const QFileInfo destination(m_request.destination);
    if (destination.exists()) {
        fail(QStringLiteral("The local destination directory already exists."),
             m_request.destination);
        return;
    }
    if (!destination.dir().exists()) {
        fail(QStringLiteral("The parent of the local destination does not exist."),
             m_request.destination);
        return;
    }
    m_localDestinationRoot = QDir::cleanPath(destination.absoluteFilePath());
    m_request.destination = m_localDestinationRoot;
    m_progress.destination = m_localDestinationRoot;
    m_pendingRemoteDirectories.enqueue({m_request.source, m_request.destination});
    m_phase = Phase::DiscoverRemoteOpen;
}

void TransferDirectoryJob::discoverUploadEntry()
{
    if (!m_localIterator->hasNext()) {
        finishDiscovery();
        return;
    }

    const QFileInfo entry = m_localIterator->nextFileInfo();
    const QString relative = QDir(m_request.source).relativeFilePath(entry.filePath());
    const QString destination = remotePathForRelative(m_request.destination, relative);
    if (destination.isEmpty()) {
        fail(QStringLiteral("The entry name is not valid for a remote path."), entry.filePath());
        return;
    }
    if (entry.isSymbolicLink()) {
        fail(QStringLiteral("Symbolic links are not transferred."), entry.filePath());
        return;
    }
    if (entry.isDir()) {
        m_directories.push_back(destination);
        return;
    }
    if (!entry.isFile()) {
        fail(QStringLiteral("Only regular files and directories can be transferred."),
             entry.filePath());
        return;
    }
    const quint64 size = static_cast<quint64>(entry.size());
    m_files.push_back({entry.filePath(), destination});
    m_progress.totalBytes += size;
}

void TransferDirectoryJob::openRemoteDirectory()
{
    if (m_pendingRemoteDirectories.isEmpty()) {
        finishDiscovery();
        return;
    }
    m_currentRemoteDirectory = m_pendingRemoteDirectories.dequeue();
    const TransferBackendResult result =
        m_backend.openDirectory(m_currentRemoteDirectory.source, m_remoteDirectoryHandle);
    if (!result.succeeded()) {
        fail(backendFailure(QStringLiteral("Unable to open remote directory"),
                            m_currentRemoteDirectory.source, result),
             m_currentRemoteDirectory.source);
        return;
    }
    m_phase = Phase::DiscoverRemoteRead;
}

void TransferDirectoryJob::readRemoteDirectoryEntry()
{
    std::optional<TransferDirectoryEntry> entry;
    const TransferBackendResult result = m_backend.readDirectory(m_remoteDirectoryHandle, entry);
    if (!result.succeeded()) {
        fail(backendFailure(QStringLiteral("Unable to read remote directory"),
                            m_currentRemoteDirectory.source, result),
             m_currentRemoteDirectory.source);
        return;
    }
    if (!entry.has_value()) {
        m_phase = Phase::DiscoverRemoteClose;
        return;
    }
    if (entry->name == QStringLiteral(".") || entry->name == QStringLiteral("..")) {
        return;
    }
    if (!RemotePath::isValidName(entry->name)) {
        fail(QStringLiteral("The remote entry has an invalid name."), entry->name);
        return;
    }
    const QString remoteSource = RemotePath::join(m_currentRemoteDirectory.source, entry->name);
    const LocalDownloadPathResult localDestination = LocalDownloadPath::child(
        m_localDestinationRoot, m_currentRemoteDirectory.destination, entry->name,
        m_localPathFlavor);
    if (!localDestination.succeeded()) {
        fail(localDestination.error, remoteSource);
        return;
    }
    if (entry->node.isSymbolicLink()) {
        fail(QStringLiteral("Symbolic links are not transferred."), remoteSource);
        return;
    }
    if (entry->node.isDirectory()) {
        m_directories.push_back(localDestination.path);
        m_pendingRemoteDirectories.enqueue({remoteSource, localDestination.path});
        return;
    }
    if (!entry->node.isRegularFile()) {
        fail(QStringLiteral("The remote entry is not a regular file or directory."), remoteSource);
        return;
    }
    m_files.push_back({remoteSource, localDestination.path});
    m_progress.totalBytes += entry->node.size;
}

void TransferDirectoryJob::closeRemoteDirectory()
{
    const TransferBackendResult result = m_backend.closeDirectory(m_remoteDirectoryHandle);
    m_remoteDirectoryHandle = 0;
    if (!result.succeeded()) {
        fail(backendFailure(QStringLiteral("Unable to close remote directory"),
                            m_currentRemoteDirectory.source, result),
             m_currentRemoteDirectory.source);
        return;
    }
    m_phase = Phase::DiscoverRemoteOpen;
}

void TransferDirectoryJob::finishDiscovery()
{
    std::sort(m_directories.begin(), m_directories.end());
    std::sort(m_files.begin(), m_files.end(), [](const FileTask& left, const FileTask& right) {
        return left.destination < right.destination;
    });
    m_progress.totalFiles = static_cast<quint64>(m_files.size());
    m_phase = Phase::CreateRoot;
}

void TransferDirectoryJob::createRootDirectory()
{
    bool created = false;
    TransferBackendResult remoteResult;
    if (m_request.direction == TransferDirection::Upload) {
        remoteResult = m_backend.createDirectory(m_request.destination);
        created = remoteResult.succeeded();
    } else {
        created = QDir().mkdir(m_request.destination);
    }
    if (!created) {
        fail(m_request.direction == TransferDirection::Upload
                 ? backendFailure(QStringLiteral("Unable to create remote destination directory"),
                                  m_request.destination, remoteResult)
                 : QStringLiteral("Unable to create local destination directory."),
             m_request.destination);
        return;
    }
    m_phase = Phase::CreateDirectories;
}

void TransferDirectoryJob::createNextDirectory()
{
    if (m_directoryIndex >= m_directories.size()) {
        m_phase = Phase::TransferFiles;
        return;
    }
    const QString path = m_directories.at(m_directoryIndex);
    TransferBackendResult remoteResult;
    const bool created = m_request.direction == TransferDirection::Upload
                             ? (remoteResult = m_backend.createDirectory(path)).succeeded()
                             : QDir().mkdir(path);
    if (!created) {
        fail(m_request.direction == TransferDirection::Upload
                 ? backendFailure(QStringLiteral("Unable to create remote subdirectory"), path,
                                  remoteResult)
                 : QStringLiteral("Unable to create local destination subdirectory."),
             path);
        return;
    }
    ++m_directoryIndex;
}

void TransferDirectoryJob::updateFromChild()
{
    const TransferProgress& childProgress = m_child->progress();
    m_progress.transferredBytes = m_completedBytes + childProgress.transferredBytes;
    m_progress.bytesPerSecond = childProgress.bytesPerSecond;
    m_progress.currentItem = childProgress.source;
}

void TransferDirectoryJob::transferNextFileStep()
{
    if (m_child == nullptr) {
        if (m_fileIndex >= m_files.size()) {
            m_progress.state = TransferState::Finalizing;
            m_progress.currentItem.clear();
            m_phase = Phase::Finalize;
            return;
        }
        const FileTask& file = m_files.at(m_fileIndex);
        m_progress.state = TransferState::Transferring;
        m_progress.currentItem = file.source;
        m_child = std::make_unique<TransferFileJob>(
            m_backend,
            TransferRequest{m_request.id, m_request.direction, file.source, file.destination});
        return;
    }

    m_child->step();
    updateFromChild();
    if (!m_child->isFinished()) {
        return;
    }
    const TransferProgress childProgress = m_child->progress();
    if (childProgress.state != TransferState::Completed) {
        const QString error = childProgress.error.isEmpty()
                                  ? QStringLiteral("The child file transfer failed.")
                                  : childProgress.error;
        m_child.reset();
        fail(error, m_files.at(m_fileIndex).source);
        return;
    }
    m_completedBytes += childProgress.transferredBytes;
    m_progress.transferredBytes = m_completedBytes;
    ++m_progress.completedFiles;
    ++m_fileIndex;
    m_child.reset();
}

void TransferDirectoryJob::cancelStep()
{
    if (m_phase == Phase::CancelChild) {
        if (m_child != nullptr && !m_child->isFinished()) {
            m_child->step();
            updateFromChild();
            return;
        }
        m_child.reset();
        m_phase = Phase::CancelCloseDirectory;
        return;
    }
    if (m_remoteDirectoryHandle != 0) {
        const TransferBackendResult result = m_backend.closeDirectory(m_remoteDirectoryHandle);
        if (!result.succeeded()) {
            appendCleanupError(QStringLiteral("The remote directory handle could not be closed."));
        }
        m_remoteDirectoryHandle = 0;
    }
    m_progress.state = TransferState::Cancelled;
    m_phase = Phase::Finished;
}

void TransferDirectoryJob::step()
{
    if (isFinished() || isPaused()) {
        return;
    }
    if (m_progress.state == TransferState::Cancelling) {
        cancelStep();
        return;
    }

    switch (m_phase) {
    case Phase::Created:
        prepare();
        break;
    case Phase::DiscoverUpload:
        discoverUploadEntry();
        break;
    case Phase::DiscoverRemoteOpen:
        openRemoteDirectory();
        break;
    case Phase::DiscoverRemoteRead:
        readRemoteDirectoryEntry();
        break;
    case Phase::DiscoverRemoteClose:
        closeRemoteDirectory();
        break;
    case Phase::CreateRoot:
        createRootDirectory();
        break;
    case Phase::CreateDirectories:
        createNextDirectory();
        break;
    case Phase::TransferFiles:
        transferNextFileStep();
        break;
    case Phase::Finalize:
        m_progress.state = TransferState::Completed;
        m_phase = Phase::Finished;
        break;
    case Phase::CancelChild:
    case Phase::CancelCloseDirectory:
        cancelStep();
        break;
    case Phase::Finished:
        break;
    }
}

bool TransferDirectoryJob::requestPause()
{
    if (isFinished() || isPaused() || m_progress.state == TransferState::Queued ||
        m_progress.state == TransferState::Cancelling) {
        return false;
    }
    m_stateBeforePause = m_progress.state;
    if (m_child != nullptr) {
        static_cast<void>(m_child->requestPause());
    }
    m_progress.state = TransferState::Paused;
    return true;
}

bool TransferDirectoryJob::resume()
{
    if (!isPaused()) {
        return false;
    }
    if (m_child != nullptr && m_child->isPaused()) {
        static_cast<void>(m_child->resume());
    }
    m_progress.state = m_stateBeforePause;
    return true;
}

bool TransferDirectoryJob::requestCancel()
{
    if (isFinished() || m_progress.state == TransferState::Cancelling) {
        return false;
    }
    m_progress.state = TransferState::Cancelling;
    if (m_child != nullptr) {
        static_cast<void>(m_child->requestCancel());
        m_phase = Phase::CancelChild;
    } else {
        m_phase = Phase::CancelCloseDirectory;
    }
    return true;
}

bool TransferDirectoryJob::isFinished() const { return m_phase == Phase::Finished; }

bool TransferDirectoryJob::isPaused() const { return m_progress.state == TransferState::Paused; }

const TransferProgress& TransferDirectoryJob::progress() const { return m_progress; }

} // namespace rfm::core
