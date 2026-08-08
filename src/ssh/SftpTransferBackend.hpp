#pragma once

#include "remotefilemanager/core/RemoteTransferBackend.hpp"

#include <QHash>

struct ssh_session_struct;
struct sftp_session_struct;
struct sftp_file_struct;
struct sftp_dir_struct;

namespace rfm::ssh
{
class SftpTransferBackend final : public rfm::core::RemoteTransferBackend
{
  public:
    explicit SftpTransferBackend(sftp_session_struct* session);
    ~SftpTransferBackend() override;
    rfm::core::TransferStatResult stat(const QString& path) override;
    rfm::core::TransferBackendResult openRead(const QString& path, quint64& handle) override;
    rfm::core::TransferBackendResult openWriteExclusive(const QString& path,
                                                        quint64& handle) override;
    rfm::core::TransferBackendResult read(quint64 handle, QByteArray& data,
                                          qsizetype maximum) override;
    rfm::core::TransferBackendResult write(quint64 handle, const QByteArray& data) override;
    rfm::core::TransferBackendResult close(quint64 handle) override;
    rfm::core::TransferBackendResult createDirectory(const QString& path) override;
    rfm::core::TransferBackendResult rename(const QString& source,
                                            const QString& destination) override;
    rfm::core::TransferBackendResult remove(const QString& path) override;
    rfm::core::TransferBackendResult openDirectory(const QString& path, quint64& handle) override;
    rfm::core::TransferBackendResult
    readDirectory(quint64 handle, std::optional<rfm::core::TransferDirectoryEntry>& entry) override;
    rfm::core::TransferBackendResult closeDirectory(quint64 handle) override;

  private:
    rfm::core::TransferBackendResult result() const;
    sftp_session_struct* m_session;
    QHash<quint64, sftp_file_struct*> m_handles;
    QHash<quint64, sftp_dir_struct*> m_directoryHandles;
    quint64 m_nextHandle{1};
};
} // namespace rfm::ssh
