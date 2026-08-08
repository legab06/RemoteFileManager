#include "SftpTransferBackend.hpp"
#include <fcntl.h>
#include <libssh/sftp.h>
namespace rfm::ssh
{
namespace
{
rfm::core::TransferBackendError map(int e)
{
    if (e == SSH_FX_NO_SUCH_FILE || e == SSH_FX_NO_SUCH_PATH)
        return rfm::core::TransferBackendError::NotFound;
    if (e == SSH_FX_FILE_ALREADY_EXISTS)
        return rfm::core::TransferBackendError::AlreadyExists;
    if (e == SSH_FX_PERMISSION_DENIED)
        return rfm::core::TransferBackendError::PermissionDenied;
    return rfm::core::TransferBackendError::Io;
}
} // namespace
SftpTransferBackend::SftpTransferBackend(sftp_session s) : m_session(s) {}
SftpTransferBackend::~SftpTransferBackend()
{
    for (auto h : m_handles)
        sftp_close(h);
    for (auto h : m_directoryHandles)
        sftp_closedir(h);
}
rfm::core::TransferBackendResult SftpTransferBackend::result() const
{
    return {map(sftp_get_error(m_session)), {}};
}
rfm::core::TransferStatResult SftpTransferBackend::stat(const QString& p)
{
    auto a = sftp_lstat(m_session, p.toUtf8().constData());
    if (!a)
        return {result(), {}};
    rfm::core::TransferNodeInfo n{true, a->type == SSH_FILEXFER_TYPE_DIRECTORY,
                                  a->type == SSH_FILEXFER_TYPE_SYMLINK, a->size};
    sftp_attributes_free(a);
    return {{}, n};
}
rfm::core::TransferBackendResult SftpTransferBackend::openRead(const QString& p, quint64& id)
{
    auto h = sftp_open(m_session, p.toUtf8().constData(), O_RDONLY, 0);
    if (!h)
        return result();
    id = m_nextHandle++;
    m_handles.insert(id, h);
    return {};
}
rfm::core::TransferBackendResult SftpTransferBackend::openWriteExclusive(const QString& p,
                                                                         quint64& id)
{
    auto h = sftp_open(m_session, p.toUtf8().constData(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (!h)
        return result();
    id = m_nextHandle++;
    m_handles.insert(id, h);
    return {};
}
rfm::core::TransferBackendResult SftpTransferBackend::read(quint64 id, QByteArray& d, qsizetype max)
{
    auto h = m_handles.value(id, nullptr);
    if (!h)
        return {rfm::core::TransferBackendError::Failure,
                QStringLiteral("Invalid remote transfer handle.")};
    d.resize(max);
    auto n = sftp_read(h, d.data(), static_cast<size_t>(max));
    if (n < 0)
        return result();
    d.truncate(n);
    return {};
}
rfm::core::TransferBackendResult SftpTransferBackend::write(quint64 id, const QByteArray& d)
{
    auto h = m_handles.value(id, nullptr);
    if (!h)
        return {rfm::core::TransferBackendError::Failure,
                QStringLiteral("Invalid remote transfer handle.")};
    auto n = sftp_write(h, d.constData(), static_cast<size_t>(d.size()));
    return n == d.size() ? rfm::core::TransferBackendResult{} : result();
}
rfm::core::TransferBackendResult SftpTransferBackend::close(quint64 id)
{
    if (!m_handles.contains(id))
        return {rfm::core::TransferBackendError::Failure,
                QStringLiteral("Invalid remote transfer handle.")};
    auto h = m_handles.take(id);
    return sftp_close(h) == SSH_OK ? rfm::core::TransferBackendResult{} : result();
}
rfm::core::TransferBackendResult SftpTransferBackend::createDirectory(const QString& p)
{
    return sftp_mkdir(m_session, p.toUtf8().constData(), 0755) == SSH_OK
               ? rfm::core::TransferBackendResult{}
               : result();
}
rfm::core::TransferBackendResult SftpTransferBackend::rename(const QString& a, const QString& b)
{
    return sftp_rename(m_session, a.toUtf8().constData(), b.toUtf8().constData()) == SSH_OK
               ? rfm::core::TransferBackendResult{}
               : result();
}
rfm::core::TransferBackendResult SftpTransferBackend::remove(const QString& p)
{
    return sftp_unlink(m_session, p.toUtf8().constData()) == SSH_OK
               ? rfm::core::TransferBackendResult{}
               : result();
}
rfm::core::TransferBackendResult SftpTransferBackend::openDirectory(const QString& p, quint64& id)
{
    auto directory = sftp_opendir(m_session, p.toUtf8().constData());
    if (!directory)
        return result();
    id = m_nextHandle++;
    m_directoryHandles.insert(id, directory);
    return {};
}
rfm::core::TransferBackendResult
SftpTransferBackend::readDirectory(quint64 id,
                                   std::optional<rfm::core::TransferDirectoryEntry>& entry)
{
    auto directory = m_directoryHandles.value(id, nullptr);
    if (!directory)
        return {rfm::core::TransferBackendError::Failure,
                QStringLiteral("Invalid remote directory handle.")};
    auto attributes = sftp_readdir(m_session, directory);
    if (!attributes) {
        entry.reset();
        return sftp_dir_eof(directory) != 0 ? rfm::core::TransferBackendResult{} : result();
    }
    entry = rfm::core::TransferDirectoryEntry{
        QString::fromUtf8(attributes->name),
        {true, attributes->type == SSH_FILEXFER_TYPE_DIRECTORY,
         attributes->type == SSH_FILEXFER_TYPE_SYMLINK, attributes->size}};
    sftp_attributes_free(attributes);
    return {};
}
rfm::core::TransferBackendResult SftpTransferBackend::closeDirectory(quint64 id)
{
    if (!m_directoryHandles.contains(id))
        return {rfm::core::TransferBackendError::Failure,
                QStringLiteral("Invalid remote directory handle.")};
    auto directory = m_directoryHandles.take(id);
    return sftp_closedir(directory) == SSH_OK ? rfm::core::TransferBackendResult{} : result();
}
} // namespace rfm::ssh
