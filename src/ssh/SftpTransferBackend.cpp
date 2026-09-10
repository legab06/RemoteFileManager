#include "SftpTransferBackend.hpp"
#include "SftpWriteLoop.hpp"
#include "SshTransportHealth.hpp"
#include <fcntl.h>
#include <libssh/sftp.h>
namespace rfm::ssh
{
namespace
{
rfm::core::TransferNodeType nodeType(const sftp_attributes attributes)
{
    switch (attributes->type) {
    case SSH_FILEXFER_TYPE_REGULAR:
        return rfm::core::TransferNodeType::RegularFile;
    case SSH_FILEXFER_TYPE_DIRECTORY:
        return rfm::core::TransferNodeType::Directory;
    case SSH_FILEXFER_TYPE_SYMLINK:
        return rfm::core::TransferNodeType::SymbolicLink;
    default:
        break;
    }
    switch (attributes->permissions & SSH_S_IFMT) {
    case SSH_S_IFIFO:
        return rfm::core::TransferNodeType::Fifo;
    case SSH_S_IFSOCK:
        return rfm::core::TransferNodeType::Socket;
    case SSH_S_IFCHR:
        return rfm::core::TransferNodeType::CharacterDevice;
    case SSH_S_IFBLK:
        return rfm::core::TransferNodeType::BlockDevice;
    case SSH_S_IFREG:
        return rfm::core::TransferNodeType::RegularFile;
    case SSH_S_IFDIR:
        return rfm::core::TransferNodeType::Directory;
    case SSH_S_IFLNK:
        return rfm::core::TransferNodeType::SymbolicLink;
    default:
        return rfm::core::TransferNodeType::Other;
    }
}

rfm::core::TransferBackendError map(int e)
{
    if (e == SSH_FX_NO_SUCH_FILE || e == SSH_FX_NO_SUCH_PATH)
        return rfm::core::TransferBackendError::NotFound;
    if (e == SSH_FX_FILE_ALREADY_EXISTS)
        return rfm::core::TransferBackendError::AlreadyExists;
    if (e == SSH_FX_PERMISSION_DENIED)
        return rfm::core::TransferBackendError::PermissionDenied;
    if (e == SSH_FX_NO_CONNECTION || e == SSH_FX_CONNECTION_LOST)
        return rfm::core::TransferBackendError::ConnectionLost;
    if (e == SSH_FX_OP_UNSUPPORTED)
        return rfm::core::TransferBackendError::Unsupported;
    return rfm::core::TransferBackendError::Io;
}

} // namespace
SftpTransferBackend::SftpTransferBackend(ssh_session sshSession, sftp_session sftpSession)
    : m_sshSession(sshSession), m_sftpSession(sftpSession)
{}
SftpTransferBackend::~SftpTransferBackend()
{
    for (auto h : m_handles)
        sftp_close(h);
    for (auto h : m_directoryHandles)
        sftp_closedir(h);
}
bool SftpTransferBackend::connectionAlive() const
{
    return !m_transportFatal && transportAlive(m_sshSession, m_sftpSession);
}
rfm::core::TransferBackendResult SftpTransferBackend::result() const
{
    rfm::core::TransferBackendError error = map(sftp_get_error(m_sftpSession));
    m_transportFatal = m_transportFatal ||
                       error == rfm::core::TransferBackendError::ConnectionLost ||
                       !transportAlive(m_sshSession, m_sftpSession);
    if (m_transportFatal) {
        error = rfm::core::TransferBackendError::ConnectionLost;
    }
    return {error, {}};
}
rfm::core::TransferStatResult SftpTransferBackend::stat(const QString& p)
{
    auto a = sftp_lstat(m_sftpSession, p.toUtf8().constData());
    if (!a)
        return {result(), {}};
    const rfm::core::TransferNodeType type = nodeType(a);
    rfm::core::TransferNodeInfo n{true, type, a->size};
    sftp_attributes_free(a);
    return {{}, n};
}
rfm::core::TransferBackendResult SftpTransferBackend::openRead(const QString& p, quint64& id)
{
    auto h = sftp_open(m_sftpSession, p.toUtf8().constData(), O_RDONLY, 0);
    if (!h)
        return result();
    id = m_nextHandle++;
    m_handles.insert(id, h);
    return {};
}
rfm::core::TransferBackendResult SftpTransferBackend::openWriteExclusive(const QString& p,
                                                                         quint64& id)
{
    auto h = sftp_open(m_sftpSession, p.toUtf8().constData(), O_WRONLY | O_CREAT | O_EXCL, 0600);
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
    const detail::SftpWriteLoopResult writeResult =
        detail::writeSftpBuffer(d, [h](const char* data, qsizetype size) {
            return static_cast<qint64>(sftp_write(h, data, static_cast<size_t>(size)));
        });
    if (writeResult == detail::SftpWriteLoopResult::Completed) {
        return {};
    }
    if (writeResult == detail::SftpWriteLoopResult::NoProgress) {
        return {rfm::core::TransferBackendError::Io,
                QStringLiteral("Remote SFTP write made no progress.")};
    }
    return result();
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
    return sftp_mkdir(m_sftpSession, p.toUtf8().constData(), 0755) == SSH_OK
               ? rfm::core::TransferBackendResult{}
               : result();
}
rfm::core::TransferBackendResult SftpTransferBackend::rename(const QString& a, const QString& b)
{
    return sftp_rename(m_sftpSession, a.toUtf8().constData(), b.toUtf8().constData()) == SSH_OK
               ? rfm::core::TransferBackendResult{}
               : result();
}
rfm::core::TransferBackendResult SftpTransferBackend::remove(const QString& p)
{
    return sftp_unlink(m_sftpSession, p.toUtf8().constData()) == SSH_OK
               ? rfm::core::TransferBackendResult{}
               : result();
}
rfm::core::TransferBackendResult SftpTransferBackend::openDirectory(const QString& p, quint64& id)
{
    auto directory = sftp_opendir(m_sftpSession, p.toUtf8().constData());
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
    auto attributes = sftp_readdir(m_sftpSession, directory);
    if (!attributes) {
        entry.reset();
        return sftp_dir_eof(directory) != 0 ? rfm::core::TransferBackendResult{} : result();
    }
    const rfm::core::TransferNodeType type = nodeType(attributes);
    entry = rfm::core::TransferDirectoryEntry{QString::fromUtf8(attributes->name),
                                              {true, type, attributes->size}};
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
