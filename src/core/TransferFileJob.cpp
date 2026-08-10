#include "remotefilemanager/core/TransferFileJob.hpp"

#include <QFileInfo>

#include <utility>

namespace rfm::core
{
namespace
{

constexpr qsizetype transferBlockSize = 65'536;

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

TransferFileJob::TransferFileJob(RemoteTransferBackend& backend, TransferRequest request)
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
                                                                    false}
{}

void TransferFileJob::appendCleanupError(const QString& error)
{
    if (error.isEmpty()) {
        return;
    }
    if (!m_progress.error.isEmpty()) {
        m_progress.error += QChar{' '};
    }
    m_progress.error += error;
}

void TransferFileJob::fail(const QString& error)
{
    m_progress.error = error;
    if (m_handle != 0) {
        const TransferBackendResult closeResult = m_backend.close(m_handle);
        if (!closeResult.succeeded()) {
            appendCleanupError(QStringLiteral("The remote handle could not be closed."));
        }
        m_handle = 0;
    }
    m_localFile.close();
    if (m_temporary != nullptr) {
        m_temporary->close();
        if (!m_temporary->remove()) {
            appendCleanupError(QStringLiteral("The local temporary file remains on disk."));
        }
    }
    if (m_ownsRemoteTemporary) {
        const TransferBackendResult removeResult = m_backend.remove(m_remoteTemporary);
        if (!removeResult.succeeded() && removeResult.error != TransferBackendError::NotFound) {
            appendCleanupError(QStringLiteral("The remote temporary file may remain."));
        }
    }
    m_progress.state = TransferState::Failed;
    m_phase = Phase::Finished;
}

void TransferFileJob::updateSpeed()
{
    qint64 elapsed = m_elapsedBeforePause;
    if (m_activeTimer.isValid()) {
        elapsed += m_activeTimer.elapsed();
    }
    if (elapsed > 0) {
        m_progress.bytesPerSecond =
            m_progress.transferredBytes * 1000 / static_cast<quint64>(elapsed);
    }
}

void TransferFileJob::step()
{
    if (isFinished() || isPaused()) {
        return;
    }

    if (m_phase == Phase::CancelCloseRemote) {
        if (m_handle != 0) {
            const TransferBackendResult result = m_backend.close(m_handle);
            if (!result.succeeded()) {
                appendCleanupError(QStringLiteral("The remote handle could not be closed."));
            }
            m_handle = 0;
        }
        m_phase = Phase::CancelCloseLocal;
        return;
    }
    if (m_phase == Phase::CancelCloseLocal) {
        m_localFile.close();
        if (m_temporary != nullptr) {
            m_temporary->close();
        }
        m_phase = Phase::CancelRemoveTemporary;
        return;
    }
    if (m_phase == Phase::CancelRemoveTemporary) {
        if (m_temporary != nullptr && !m_temporary->remove()) {
            appendCleanupError(QStringLiteral("The local temporary file remains on disk."));
        }
        if (m_ownsRemoteTemporary) {
            const TransferBackendResult result = m_backend.remove(m_remoteTemporary);
            if (!result.succeeded() && result.error != TransferBackendError::NotFound) {
                appendCleanupError(QStringLiteral("The remote temporary file may remain."));
            }
            m_ownsRemoteTemporary = false;
        }
        m_progress.state = TransferState::Cancelled;
        m_phase = Phase::Finished;
        return;
    }

    if (m_phase == Phase::Created) {
        m_progress.state = TransferState::Preparing;
        const QString remotePath = m_request.direction == TransferDirection::Upload
                                       ? m_request.destination
                                       : m_request.source;
        const TransferStatResult node = m_backend.stat(remotePath);
        if (m_request.direction == TransferDirection::Upload) {
            if (node.result.succeeded() && node.node.exists) {
                fail(QStringLiteral("The remote destination already exists: %1").arg(remotePath));
                return;
            }
            if (!node.result.succeeded() && node.result.error != TransferBackendError::NotFound) {
                fail(backendFailure(QStringLiteral("Unable to inspect remote destination"),
                                    remotePath, node.result));
                return;
            }
        } else {
            if (!node.result.succeeded()) {
                fail(backendFailure(QStringLiteral("Unable to inspect remote source"), remotePath,
                                    node.result));
                return;
            }
            if (!node.node.exists) {
                fail(QStringLiteral("The remote source was not found: %1").arg(remotePath));
                return;
            }
            if (!node.node.isRegularFile()) {
                fail(QStringLiteral("The remote source is not a regular file: %1").arg(remotePath));
                return;
            }
        }
        m_progress.totalBytes = node.node.size;
        m_phase = Phase::OpenLocal;
        return;
    }

    if (m_request.direction == TransferDirection::Upload) {
        if (m_phase == Phase::OpenLocal) {
            const QFileInfo sourceInfo(m_request.source);
            if (!sourceInfo.exists() || !sourceInfo.isFile() || sourceInfo.isSymbolicLink()) {
                fail(QStringLiteral("The local source is missing, invalid, or symbolic."));
                return;
            }
            m_localFile.setFileName(m_request.source);
            if (!m_localFile.open(QIODevice::ReadOnly)) {
                fail(QStringLiteral("Unable to read local source."));
                return;
            }
            m_progress.totalBytes = static_cast<quint64>(m_localFile.size());
            m_phase = Phase::OpenRemote;
            return;
        }
        if (m_phase == Phase::OpenRemote) {
            m_remoteTemporary =
                m_request.destination + QStringLiteral(".rfm-part-%1").arg(m_request.id);
            const TransferBackendResult result =
                m_backend.openWriteExclusive(m_remoteTemporary, m_handle);
            if (!result.succeeded()) {
                fail(backendFailure(QStringLiteral("Unable to create remote temporary file"),
                                    m_remoteTemporary, result));
                return;
            }
            m_ownsRemoteTemporary = true;
            m_progress.state = TransferState::Transferring;
            m_activeTimer.start();
            m_phase = Phase::Transfer;
            return;
        }
        if (m_phase == Phase::Transfer) {
            const QByteArray data = m_localFile.read(transferBlockSize);
            if (data.isEmpty()) {
                if (m_localFile.error() != QFileDevice::NoError) {
                    fail(QStringLiteral("Local read failed."));
                    return;
                }
                m_progress.state = TransferState::Finalizing;
                m_phase = Phase::Close;
                return;
            }
            const TransferBackendResult result = m_backend.write(m_handle, data);
            if (!result.succeeded()) {
                fail(backendFailure(QStringLiteral("Unable to write remote temporary file"),
                                    m_remoteTemporary, result));
                return;
            }
            m_progress.transferredBytes += static_cast<quint64>(data.size());
            updateSpeed();
            return;
        }
    } else {
        if (m_phase == Phase::OpenLocal) {
            if (QFileInfo::exists(m_request.destination)) {
                fail(QStringLiteral("Local destination exists."));
                return;
            }
            m_temporary = std::make_unique<QTemporaryFile>(m_request.destination +
                                                           QStringLiteral(".rfm-part-XXXXXX"));
            if (!m_temporary->open()) {
                fail(QStringLiteral("Unable to open local temporary file."));
                return;
            }
            m_phase = Phase::OpenRemote;
            return;
        }
        if (m_phase == Phase::OpenRemote) {
            const TransferBackendResult result = m_backend.openRead(m_request.source, m_handle);
            if (!result.succeeded()) {
                fail(backendFailure(QStringLiteral("Unable to open remote source"),
                                    m_request.source, result));
                return;
            }
            m_progress.state = TransferState::Transferring;
            m_activeTimer.start();
            m_phase = Phase::Transfer;
            return;
        }
        if (m_phase == Phase::Transfer) {
            QByteArray data;
            const TransferBackendResult result = m_backend.read(m_handle, data, transferBlockSize);
            if (!result.succeeded()) {
                fail(backendFailure(QStringLiteral("Unable to read remote source"),
                                    m_request.source, result));
                return;
            }
            if (data.isEmpty()) {
                m_progress.state = TransferState::Finalizing;
                m_phase = Phase::Close;
                return;
            }
            if (m_temporary->write(data) != data.size()) {
                fail(QStringLiteral("Local write failed."));
                return;
            }
            m_progress.transferredBytes += static_cast<quint64>(data.size());
            updateSpeed();
            return;
        }
    }

    if (m_phase == Phase::Close) {
        if (m_handle != 0) {
            const TransferBackendResult result = m_backend.close(m_handle);
            m_handle = 0;
            if (!result.succeeded()) {
                fail(backendFailure(QStringLiteral("Unable to close remote file"),
                                    m_request.direction == TransferDirection::Upload
                                        ? m_remoteTemporary
                                        : m_request.source,
                                    result));
                return;
            }
        }
        m_localFile.close();
        if (m_temporary != nullptr) {
            m_temporary->close();
        }
        m_phase = Phase::Finalize;
        return;
    }
    if (m_phase == Phase::Finalize) {
        bool promoted = false;
        TransferBackendResult remotePromotion;
        if (m_request.direction == TransferDirection::Upload) {
            remotePromotion = m_backend.rename(m_remoteTemporary, m_request.destination);
            promoted = remotePromotion.succeeded();
            if (promoted) {
                m_ownsRemoteTemporary = false;
            }
        } else {
            promoted = QFile::rename(m_temporary->fileName(), m_request.destination);
        }
        if (!promoted) {
            fail(m_request.direction == TransferDirection::Upload
                     ? backendFailure(QStringLiteral("Unable to promote remote temporary file"),
                                      m_request.destination, remotePromotion)
                     : QStringLiteral("Unable to promote local temporary file: %1")
                           .arg(m_request.destination));
            return;
        }
        updateSpeed();
        m_progress.state = TransferState::Completed;
        m_phase = Phase::Finished;
    }
}

bool TransferFileJob::requestPause()
{
    if (isFinished() || isPaused() || m_progress.state == TransferState::Queued ||
        m_progress.state == TransferState::Cancelling) {
        return false;
    }
    m_stateBeforePause = m_progress.state;
    if (m_activeTimer.isValid()) {
        m_elapsedBeforePause += m_activeTimer.elapsed();
        m_activeTimer.invalidate();
    }
    m_progress.state = TransferState::Paused;
    return true;
}

bool TransferFileJob::resume()
{
    if (!isPaused()) {
        return false;
    }
    m_progress.state = m_stateBeforePause;
    if (m_progress.state == TransferState::Transferring) {
        m_activeTimer.start();
    }
    return true;
}

bool TransferFileJob::requestCancel()
{
    if (isFinished() || m_progress.state == TransferState::Cancelling) {
        return false;
    }
    if (m_activeTimer.isValid()) {
        m_elapsedBeforePause += m_activeTimer.elapsed();
        m_activeTimer.invalidate();
    }
    m_progress.state = TransferState::Cancelling;
    m_phase = Phase::CancelCloseRemote;
    return true;
}

bool TransferFileJob::isFinished() const { return m_phase == Phase::Finished; }

bool TransferFileJob::isPaused() const { return m_progress.state == TransferState::Paused; }

const TransferProgress& TransferFileJob::progress() const { return m_progress; }

} // namespace rfm::core
