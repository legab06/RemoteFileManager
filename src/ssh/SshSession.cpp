#include "remotefilemanager/ssh/SshSession.hpp"

#include "SftpTransferBackend.hpp"
#include "remotefilemanager/core/RemotePath.hpp"
#include "remotefilemanager/core/ServerSideCopyJob.hpp"
#include "remotefilemanager/core/Storage.hpp"
#include "remotefilemanager/core/TransferDirectoryJob.hpp"
#include "remotefilemanager/core/TransferFileJob.hpp"
#include "remotefilemanager/core/TransferJob.hpp"
#include "remotefilemanager/core/TransferQueue.hpp"
#include "remotefilemanager/ssh/RemoteCopyCommand.hpp"
#include "remotefilemanager/ssh/RemoteStorageScanner.hpp"
#include "remotefilemanager/ssh/RemoteVolumeService.hpp"

#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QMetaObject>
#include <QQueue>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <utility>

namespace
{

rfm::core::RemoteBackendError backendError(int sftpError)
{
    switch (sftpError) {
    case SSH_FX_OK:
        return rfm::core::RemoteBackendError::None;
    case SSH_FX_NO_SUCH_FILE:
    case SSH_FX_NO_SUCH_PATH:
        return rfm::core::RemoteBackendError::NotFound;
    case SSH_FX_PERMISSION_DENIED:
        return rfm::core::RemoteBackendError::PermissionDenied;
    case SSH_FX_FILE_ALREADY_EXISTS:
        return rfm::core::RemoteBackendError::AlreadyExists;
    case SSH_FX_OP_UNSUPPORTED:
        return rfm::core::RemoteBackendError::Unsupported;
    default:
        return rfm::core::RemoteBackendError::Failure;
    }
}

bool isFatalSftpError(int sftpError)
{
    return sftpError == SSH_FX_NO_CONNECTION || sftpError == SSH_FX_CONNECTION_LOST;
}

rfm::ssh::RemoteStorageError storageError(ssh_session session, sftp_session sftp)
{
    const int error = sftp == nullptr ? SSH_FX_NO_CONNECTION : sftp_get_error(sftp);
    if (session == nullptr || ssh_is_connected(session) == 0 || isFatalSftpError(error)) {
        return rfm::ssh::RemoteStorageError::ConnectionLost;
    }
    switch (error) {
    case SSH_FX_NO_SUCH_FILE:
    case SSH_FX_NO_SUCH_PATH:
        return rfm::ssh::RemoteStorageError::NotFound;
    case SSH_FX_PERMISSION_DENIED:
        return rfm::ssh::RemoteStorageError::PermissionDenied;
    default:
        return rfm::ssh::RemoteStorageError::OtherError;
    }
}

QString resolveRemoteLink(const QString& linkPath, const QString& target)
{
    if (target.startsWith(QChar{'/'})) {
        return rfm::core::RemotePath::normalize(target);
    }
    return rfm::core::RemotePath::normalize(rfm::core::RemotePath::parent(linkPath) + QChar{'/'} +
                                            target);
}

class SftpRemoteStorageReader final : public rfm::ssh::RemoteStorageReader
{
  public:
    SftpRemoteStorageReader(ssh_session session, sftp_session sftp)
        : m_session(session), m_sftp(sftp)
    {}

    ~SftpRemoteStorageReader() override
    {
        endMountInfo();
        endFileSystemLabels();
    }

    rfm::ssh::RemoteStorageError beginMountInfo() override
    {
        endMountInfo();
        const QByteArray encodedPath = QByteArrayLiteral("/proc/self/mountinfo");
        m_mountInfoFile = sftp_open(m_sftp, encodedPath.constData(), O_RDONLY, 0);
        return m_mountInfoFile == nullptr ? storageError(m_session, m_sftp)
                                          : rfm::ssh::RemoteStorageError::None;
    }

    rfm::ssh::RemoteStorageChunkResult readMountInfoChunk(qsizetype maximumBytes) override
    {
        if (m_mountInfoFile == nullptr || maximumBytes <= 0) {
            return {{}, rfm::ssh::RemoteStorageError::OtherError, false};
        }
        char buffer[4096];
        const size_t requested =
            static_cast<size_t>(std::min(maximumBytes, static_cast<qsizetype>(sizeof(buffer))));
        const ssize_t count = sftp_read(m_mountInfoFile, buffer, requested);
        if (count < 0) {
            return {{}, storageError(m_session, m_sftp), false};
        }
        if (count == 0) {
            return {{}, rfm::ssh::RemoteStorageError::None, true};
        }
        return {QByteArray(buffer, static_cast<qsizetype>(count)),
                rfm::ssh::RemoteStorageError::None, false};
    }

    void endMountInfo() override
    {
        if (m_mountInfoFile != nullptr) {
            sftp_close(m_mountInfoFile);
            m_mountInfoFile = nullptr;
        }
    }

    rfm::ssh::RemoteStorageByteResult readFile(const QString& path, qsizetype maximumBytes) override
    {
        if (maximumBytes <= 0) {
            return {{}, rfm::ssh::RemoteStorageError::OtherError, false};
        }
        const QByteArray encodedPath = path.toUtf8();
        sftp_file file = sftp_open(m_sftp, encodedPath.constData(), O_RDONLY, 0);
        if (file == nullptr) {
            return {{}, storageError(m_session, m_sftp), false};
        }
        QByteArray contents;
        char buffer[4096];
        const qsizetype readLimit = maximumBytes + 1;
        while (contents.size() < readLimit) {
            const size_t requested = static_cast<size_t>(std::min<qsizetype>(
                static_cast<qsizetype>(sizeof(buffer)), readLimit - contents.size()));
            const ssize_t count = sftp_read(file, buffer, requested);
            if (count < 0) {
                const rfm::ssh::RemoteStorageError error = storageError(m_session, m_sftp);
                sftp_close(file);
                return {{}, error, false};
            }
            if (count == 0) {
                break;
            }
            contents.append(buffer, static_cast<qsizetype>(count));
        }
        sftp_close(file);
        const bool truncated = contents.size() > maximumBytes;
        if (truncated) {
            contents.truncate(maximumBytes);
        }
        return {std::move(contents), rfm::ssh::RemoteStorageError::None, truncated};
    }

    rfm::ssh::RemoteStorageStringResult readLink(const QString& path) override
    {
        const QByteArray encodedPath = path.toUtf8();
        char* const target = sftp_readlink(m_sftp, encodedPath.constData());
        if (target == nullptr) {
            return {{}, storageError(m_session, m_sftp)};
        }
        const QString result = QString::fromUtf8(target);
        ssh_string_free_char(target);
        return {result, rfm::ssh::RemoteStorageError::None};
    }

    rfm::ssh::RemoteStorageError beginFileSystemLabels() override
    {
        endFileSystemLabels();
        const QByteArray encodedDirectory = labelDirectory().toUtf8();
        m_labelDirectory = sftp_opendir(m_sftp, encodedDirectory.constData());
        return m_labelDirectory == nullptr ? storageError(m_session, m_sftp)
                                           : rfm::ssh::RemoteStorageError::None;
    }

    rfm::ssh::RemoteStorageLabelResult nextFileSystemLabel() override
    {
        if (m_labelDirectory == nullptr) {
            return {{}, {}, rfm::ssh::RemoteStorageError::OtherError, true};
        }
        sftp_attributes attributes = sftp_readdir(m_sftp, m_labelDirectory);
        if (attributes == nullptr) {
            if (sftp_dir_eof(m_labelDirectory) != 0) {
                return {{}, {}, rfm::ssh::RemoteStorageError::None, true};
            }
            return {{}, {}, storageError(m_session, m_sftp), false};
        }
        const QString label =
            attributes->name == nullptr ? QString{} : QString::fromUtf8(attributes->name).trimmed();
        sftp_attributes_free(attributes);
        if (label.isEmpty() || label == QStringLiteral(".") || label == QStringLiteral("..")) {
            return {};
        }
        const QString linkPath = labelDirectory() + QChar{'/'} + label;
        const rfm::ssh::RemoteStorageStringResult target = readLink(linkPath);
        if (target.error == rfm::ssh::RemoteStorageError::ConnectionLost ||
            target.error == rfm::ssh::RemoteStorageError::Cancelled) {
            return {{}, {}, target.error, false};
        }
        if (target.error != rfm::ssh::RemoteStorageError::None) {
            return {};
        }
        return {label, resolveRemoteLink(linkPath, target.value),
                rfm::ssh::RemoteStorageError::None, false};
    }

    void endFileSystemLabels() override
    {
        if (m_labelDirectory != nullptr) {
            sftp_closedir(m_labelDirectory);
            m_labelDirectory = nullptr;
        }
    }

    rfm::ssh::RemoteStorageTopologyResult readTopologyNode(const QString& path) override
    {
        const QByteArray encodedPath = path.toUtf8();
        sftp_attributes attributes = sftp_lstat(m_sftp, encodedPath.constData());
        if (attributes == nullptr) {
            return {{}, storageError(m_session, m_sftp), false};
        }
        sftp_attributes_free(attributes);

        rfm::core::StorageTopologyNode node;
        bool reliable = true;
        const rfm::ssh::RemoteStorageByteResult removable =
            readFile(path + QStringLiteral("/removable"), 8);
        if (removable.error == rfm::ssh::RemoteStorageError::ConnectionLost) {
            return {{}, removable.error, false};
        }
        if (removable.error == rfm::ssh::RemoteStorageError::PermissionDenied ||
            removable.error == rfm::ssh::RemoteStorageError::OtherError || removable.truncated) {
            reliable = false;
        }
        node.removable = removable.error == rfm::ssh::RemoteStorageError::None &&
                         removable.data.trimmed() == QByteArrayLiteral("1");

        const rfm::ssh::RemoteStorageStringResult subsystem =
            readLink(path + QStringLiteral("/subsystem"));
        if (subsystem.error == rfm::ssh::RemoteStorageError::ConnectionLost) {
            return {{}, subsystem.error, false};
        }
        if (subsystem.error == rfm::ssh::RemoteStorageError::PermissionDenied ||
            subsystem.error == rfm::ssh::RemoteStorageError::OtherError) {
            reliable = false;
        }
        if (subsystem.error == rfm::ssh::RemoteStorageError::None) {
            node.subsystem = rfm::core::RemotePath::fileName(subsystem.value);
        }

        rfm::ssh::RemoteStorageByteResult model = readFile(path + QStringLiteral("/model"), 256);
        if (model.error == rfm::ssh::RemoteStorageError::ConnectionLost) {
            return {{}, model.error, false};
        }
        if (model.error != rfm::ssh::RemoteStorageError::None || model.data.trimmed().isEmpty()) {
            model = readFile(path + QStringLiteral("/device/model"), 256);
            if (model.error == rfm::ssh::RemoteStorageError::ConnectionLost) {
                return {{}, model.error, false};
            }
        }
        if (model.error == rfm::ssh::RemoteStorageError::None) {
            node.deviceModel = QString::fromUtf8(model.data.trimmed());
        }

        // Optional children may legitimately be absent. Revalidate the node so
        // that NotFound can also reveal removal during this composite read.
        attributes = sftp_lstat(m_sftp, encodedPath.constData());
        if (attributes == nullptr) {
            const rfm::ssh::RemoteStorageError error = storageError(m_session, m_sftp);
            if (error == rfm::ssh::RemoteStorageError::ConnectionLost) {
                return {{}, error, false};
            }
            reliable = false;
        } else {
            sftp_attributes_free(attributes);
        }
        return {std::move(node), rfm::ssh::RemoteStorageError::None, reliable};
    }

    rfm::ssh::RemoteStorageStringResult deviceIdentity(const QString& device) override
    {
        const QString normalized = rfm::core::RemotePath::normalize(device);
        if (!normalized.startsWith(QChar{'/'})) {
            return {normalized, rfm::ssh::RemoteStorageError::None};
        }
        const rfm::ssh::RemoteStorageStringResult target = readLink(normalized);
        if (target.error == rfm::ssh::RemoteStorageError::ConnectionLost) {
            return target;
        }
        return {target.error == rfm::ssh::RemoteStorageError::None
                    ? resolveRemoteLink(normalized, target.value)
                    : normalized,
                rfm::ssh::RemoteStorageError::None};
    }

    rfm::ssh::RemoteStorageSizeResult storageSize(const QString& mountPoint) override
    {
        const QByteArray encodedPath = mountPoint.toUtf8();
        sftp_statvfs_t attributes = sftp_statvfs(m_sftp, encodedPath.constData());
        if (attributes == nullptr) {
            return {0, storageError(m_session, m_sftp)};
        }
        quint64 bytes = 0;
        if (attributes->f_frsize > 0 &&
            attributes->f_blocks <= std::numeric_limits<quint64>::max() / attributes->f_frsize) {
            bytes = attributes->f_blocks * attributes->f_frsize;
        }
        sftp_statvfs_free(attributes);
        return {bytes, rfm::ssh::RemoteStorageError::None};
    }

    bool connectionAlive() const override
    {
        return m_session != nullptr && m_sftp != nullptr && ssh_is_connected(m_session) != 0;
    }

  private:
    static QString labelDirectory() { return QStringLiteral("/dev/disk/by-label"); }

    ssh_session m_session{nullptr};
    sftp_session m_sftp{nullptr};
    sftp_file m_mountInfoFile{nullptr};
    sftp_dir m_labelDirectory{nullptr};
};

class SftpBackend final : public rfm::core::RemoteFileBackend
{
  public:
    explicit SftpBackend(sftp_session sftp) : m_sftp(sftp) {}

    rfm::core::RemoteProbeResult probe(const QString& path) override
    {
        const QByteArray encoded = path.toUtf8();
        sftp_attributes attributes = sftp_lstat(m_sftp, encoded.constData());
        if (attributes == nullptr) {
            return {{backendError(sftp_get_error(m_sftp)), {}}, {}};
        }
        const bool directory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
        sftp_attributes_free(attributes);
        return {{}, {true, directory}};
    }

    rfm::core::RemoteDirectoryResult list(const QString& path) override
    {
        const QByteArray encoded = path.toUtf8();
        sftp_dir directory = sftp_opendir(m_sftp, encoded.constData());
        if (directory == nullptr) {
            return {{backendError(sftp_get_error(m_sftp)), {}}, {}};
        }
        QList<QPair<QString, bool>> entries;
        while (sftp_attributes attributes = sftp_readdir(m_sftp, directory)) {
            const QString name = QString::fromUtf8(attributes->name);
            if (name != QStringLiteral(".") && name != QStringLiteral("..")) {
                entries.push_back({name, attributes->type == SSH_FILEXFER_TYPE_DIRECTORY});
            }
            sftp_attributes_free(attributes);
        }
        const int error = sftp_dir_eof(directory) == 0 ? sftp_get_error(m_sftp) : SSH_FX_OK;
        sftp_closedir(directory);
        return {{backendError(error), {}}, entries};
    }

    rfm::core::RemoteBackendResult createDirectory(const QString& path) override
    {
        const QByteArray encoded = path.toUtf8();
        if (sftp_mkdir(m_sftp, encoded.constData(), 0755) == SSH_OK) {
            return {};
        }
        return {backendError(sftp_get_error(m_sftp)), {}};
    }

    rfm::core::RemoteBackendResult rename(const QString& source,
                                          const QString& destination) override
    {
        const QByteArray encodedSource = source.toUtf8();
        const QByteArray encodedDestination = destination.toUtf8();
        if (sftp_rename(m_sftp, encodedSource.constData(), encodedDestination.constData()) ==
            SSH_OK) {
            return {};
        }
        return {backendError(sftp_get_error(m_sftp)), {}};
    }

    rfm::core::RemoteBackendResult removeFile(const QString& path) override
    {
        const QByteArray encoded = path.toUtf8();
        if (sftp_unlink(m_sftp, encoded.constData()) == SSH_OK) {
            return {};
        }
        return {backendError(sftp_get_error(m_sftp)), {}};
    }

    rfm::core::RemoteBackendResult removeDirectory(const QString& path) override
    {
        const QByteArray encoded = path.toUtf8();
        if (sftp_rmdir(m_sftp, encoded.constData()) == SSH_OK) {
            return {};
        }
        return {backendError(sftp_get_error(m_sftp)), {}};
    }

    rfm::core::RemoteBackendResult copyOnServer(const QString&, const QString&, bool) override
    {
        return {rfm::core::RemoteBackendError::Unsupported,
                QCoreApplication::translate("SftpBackend",
                                            "Server-side copies use the cooperative copy worker.")};
    }

  private:
    sftp_session m_sftp;
};

class SshServerSideCopyBackend final : public rfm::core::ServerSideCopyBackend
{
  public:
    SshServerSideCopyBackend(ssh_session session, sftp_session sftp)
        : m_session(session), m_sftp(sftp)
    {}

    ~SshServerSideCopyBackend() override { closeChannel(); }

    rfm::core::RemoteProbeResult probe(const QString& path) override
    {
        const QByteArray encoded = path.toUtf8();
        sftp_attributes attributes = sftp_lstat(m_sftp, encoded.constData());
        if (attributes == nullptr) {
            return {{backendError(sftp_get_error(m_sftp)), {}}, {}};
        }
        const bool directory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
        sftp_attributes_free(attributes);
        return {{}, {true, directory}};
    }

    rfm::core::RemoteBackendResult startCopy(const QString& source, const QString& destination,
                                             bool recursive) override
    {
        closeChannel();
        const QString command = rfm::ssh::RemoteCopyCommand::build(source, destination, recursive);
        if (command.isEmpty()) {
            return {rfm::core::RemoteBackendError::InvalidPath, {}};
        }
        m_channel = ssh_channel_new(m_session);
        if (m_channel == nullptr) {
            return {rfm::core::RemoteBackendError::Failure,
                    QCoreApplication::translate("SshServerSideCopyBackend",
                                                "Unable to open an SSH channel.")};
        }
        m_errorOutput.clear();
        m_exitStatePolls = 0;
        m_exitStateReceived = false;
        m_exitCode = UINT32_MAX;
        m_cancellationPhase = CancellationPhase::NotRequested;
        m_cancellationTimer.invalidate();
        m_channelCallbacks = {};
        ssh_callbacks_init(&m_channelCallbacks);
        m_channelCallbacks.userdata = this;
        m_channelCallbacks.channel_exit_status_function = &handleExitStatus;
        m_channelCallbacks.channel_exit_signal_function = &handleExitSignal;
        if (ssh_set_channel_callbacks(m_channel, &m_channelCallbacks) != SSH_OK) {
            closeChannel();
            return {rfm::core::RemoteBackendError::Failure,
                    QCoreApplication::translate("SshServerSideCopyBackend",
                                                "Unable to monitor the remote copy channel.")};
        }
        if (ssh_channel_open_session(m_channel) != SSH_OK) {
            closeChannel();
            return {rfm::core::RemoteBackendError::Unsupported,
                    QCoreApplication::translate("SshServerSideCopyBackend",
                                                "The server rejected remote SSH commands.")};
        }
        const QByteArray encodedCommand = command.toUtf8();
        if (ssh_channel_request_exec(m_channel, encodedCommand.constData()) != SSH_OK) {
            closeChannel();
            return {rfm::core::RemoteBackendError::Unsupported,
                    QCoreApplication::translate("SshServerSideCopyBackend",
                                                "Remote copy is not supported by this server.")};
        }
        return {};
    }

    std::optional<rfm::core::RemoteBackendResult> pollCopy() override
    {
        if (m_channel == nullptr) {
            return rfm::core::RemoteBackendResult{
                rfm::core::RemoteBackendError::Failure,
                QCoreApplication::translate("SshServerSideCopyBackend",
                                            "The remote copy channel is not active.")};
        }
        char buffer[512];
        const int errorBytes = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 1);
        if (errorBytes > 0 && m_errorOutput.size() < 2048) {
            const int remaining = 2048 - static_cast<int>(m_errorOutput.size());
            m_errorOutput.append(buffer, std::min(errorBytes, remaining));
        } else if (errorBytes == SSH_ERROR) {
            closeChannel();
            return rfm::core::RemoteBackendResult{
                rfm::core::RemoteBackendError::Failure,
                QCoreApplication::translate("SshServerSideCopyBackend",
                                            "Unable to read the remote copy result.")};
        }
        const int outputBytes = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 0);
        if (outputBytes == SSH_ERROR) {
            closeChannel();
            return rfm::core::RemoteBackendResult{
                rfm::core::RemoteBackendError::Failure,
                QCoreApplication::translate("SshServerSideCopyBackend",
                                            "Unable to drain the remote copy channel.")};
        }
        if (ssh_channel_is_eof(m_channel) == 0) {
            return std::nullopt;
        }

        if (!m_exitStateReceived && ++m_exitStatePolls < 100) {
            return std::nullopt;
        }
        const QString detail = QString::fromUtf8(m_errorOutput).trimmed();
        const bool exitStateReceived = m_exitStateReceived;
        const uint32_t exitCode = m_exitCode;
        closeChannel();
        if (exitStateReceived && exitCode == 0) {
            return rfm::core::RemoteBackendResult{};
        }
        if (exitCode == 126 || exitCode == 127) {
            return rfm::core::RemoteBackendResult{
                rfm::core::RemoteBackendError::Unsupported,
                QCoreApplication::translate("SshServerSideCopyBackend",
                                            "The 'cp' command is not available on the server.")};
        }
        return rfm::core::RemoteBackendResult{
            rfm::core::RemoteBackendError::Failure,
            detail.isEmpty()
                ? QCoreApplication::translate("SshServerSideCopyBackend", "Remote copy failed.")
                : detail};
    }

    std::optional<rfm::core::RemoteBackendResult> requestCopyCancellation() override
    {
        if (m_channel == nullptr) {
            return rfm::core::RemoteBackendResult{};
        }

        if (m_cancellationPhase == CancellationPhase::NotRequested) {
            m_cancellationPhase = CancellationPhase::RequestingTermination;
            m_cancellationTimer.start();
        }
        if (m_cancellationPhase == CancellationPhase::RequestingTermination) {
            if (ssh_channel_is_eof(m_channel) != 0 || ssh_channel_is_closed(m_channel) != 0) {
                m_cancellationPhase = CancellationPhase::AwaitingTermination;
                m_cancellationTimer.restart();
                return rfm::core::RemoteBackendResult{};
            }
            const int signalResult = ssh_channel_request_send_signal(m_channel, "TERM");
            if (signalResult == SSH_OK) {
                m_cancellationPhase = CancellationPhase::AwaitingTermination;
                m_cancellationTimer.restart();
                return rfm::core::RemoteBackendResult{};
            }
            if (m_cancellationTimer.elapsed() < terminationRequestTimeoutMs) {
                return std::nullopt;
            }
            closeChannel();
            return rfm::core::RemoteBackendResult{
                rfm::core::RemoteBackendError::Failure,
                QCoreApplication::translate(
                    "SshServerSideCopyBackend",
                    "Timed out while requesting termination of the remote copy.")};
        }
        return rfm::core::RemoteBackendResult{};
    }

    std::optional<rfm::core::RemoteBackendResult> pollCopyCancellation() override
    {
        if (m_channel == nullptr) {
            return rfm::core::RemoteBackendResult{};
        }
        char buffer[512];
        const int errorBytes = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 1);
        const int outputBytes = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 0);
        if (errorBytes == SSH_ERROR || outputBytes == SSH_ERROR) {
            closeChannel();
            return rfm::core::RemoteBackendResult{
                rfm::core::RemoteBackendError::Failure,
                QCoreApplication::translate("SshServerSideCopyBackend",
                                            "Unable to confirm termination of the remote copy.")};
        }
        if (ssh_channel_is_eof(m_channel) != 0 || ssh_channel_is_closed(m_channel) != 0) {
            closeChannel();
            return rfm::core::RemoteBackendResult{};
        }
        if (m_cancellationTimer.elapsed() < terminationCompletionTimeoutMs) {
            return std::nullopt;
        }

        closeChannel();
        return rfm::core::RemoteBackendResult{
            rfm::core::RemoteBackendError::Failure,
            QCoreApplication::translate(
                "SshServerSideCopyBackend",
                "The remote copy did not terminate within the cancellation timeout.")};
    }

  private:
    enum class CancellationPhase { NotRequested, RequestingTermination, AwaitingTermination };

    static constexpr qint64 terminationRequestTimeoutMs = 1000;
    static constexpr qint64 terminationCompletionTimeoutMs = 5000;

    static void handleExitStatus(ssh_session, ssh_channel, int exitStatus, void* userData)
    {
        auto* backend = static_cast<SshServerSideCopyBackend*>(userData);
        backend->m_exitStateReceived = true;
        backend->m_exitCode = exitStatus >= 0 ? static_cast<uint32_t>(exitStatus) : UINT32_MAX;
    }

    static void handleExitSignal(ssh_session, ssh_channel, const char*, int, const char*,
                                 const char*, void* userData)
    {
        auto* backend = static_cast<SshServerSideCopyBackend*>(userData);
        backend->m_exitStateReceived = true;
        backend->m_exitCode = UINT32_MAX;
    }

    void closeChannel()
    {
        if (m_channel == nullptr) {
            return;
        }
        static_cast<void>(ssh_channel_close(m_channel));
        ssh_channel_free(m_channel);
        m_channel = nullptr;
    }

    ssh_session m_session;
    sftp_session m_sftp;
    ssh_channel m_channel{nullptr};
    QByteArray m_errorOutput;
    int m_exitStatePolls{0};
    bool m_exitStateReceived{false};
    uint32_t m_exitCode{UINT32_MAX};
    ssh_channel_callbacks_struct m_channelCallbacks{};
    CancellationPhase m_cancellationPhase{CancellationPhase::NotRequested};
    QElapsedTimer m_cancellationTimer;
};

struct SshCommandPollResult {
    std::optional<rfm::core::VolumeCommandResult> result;
    bool connectionLost{false};
};

class SshCommandProcess final
{
  public:
    explicit SshCommandProcess(ssh_session session) : m_session(session) {}
    ~SshCommandProcess() { closeChannel(); }

    bool start(const QString& command)
    {
        if (m_session == nullptr || ssh_is_connected(m_session) == 0 || command.isEmpty()) {
            return false;
        }
        m_channel = ssh_channel_new(m_session);
        if (m_channel == nullptr) {
            return false;
        }
        m_callbacks = {};
        ssh_callbacks_init(&m_callbacks);
        m_callbacks.userdata = this;
        m_callbacks.channel_exit_status_function = &handleExitStatus;
        m_callbacks.channel_exit_signal_function = &handleExitSignal;
        if (ssh_set_channel_callbacks(m_channel, &m_callbacks) != SSH_OK ||
            ssh_channel_open_session(m_channel) != SSH_OK) {
            closeChannel();
            return false;
        }
        const QByteArray encoded = command.toUtf8();
        if (ssh_channel_request_exec(m_channel, encoded.constData()) != SSH_OK) {
            closeChannel();
            return false;
        }
        m_timer.start();
        return true;
    }

    SshCommandPollResult poll()
    {
        if (m_channel == nullptr || m_session == nullptr || ssh_is_connected(m_session) == 0) {
            closeChannel();
            return {{}, true};
        }
        char buffer[4096];
        const int errorBytes = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 1);
        if (errorBytes == SSH_ERROR) {
            const bool lost = ssh_is_connected(m_session) == 0;
            const QString output = QString::fromUtf8(m_standardOutput);
            const QString error = QString::fromUtf8(m_standardError);
            closeChannel();
            return lost ? SshCommandPollResult{{}, true}
                        : SshCommandPollResult{
                              rfm::core::VolumeCommandResult{true, false, true, -1, output, error},
                              false};
        }
        appendBounded(m_standardError, buffer, errorBytes);
        const int outputBytes = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 0);
        if (outputBytes == SSH_ERROR) {
            const bool lost = ssh_is_connected(m_session) == 0;
            const QString output = QString::fromUtf8(m_standardOutput);
            const QString error = QString::fromUtf8(m_standardError);
            closeChannel();
            return lost ? SshCommandPollResult{{}, true}
                        : SshCommandPollResult{
                              rfm::core::VolumeCommandResult{true, false, true, -1, output, error},
                              false};
        }
        appendBounded(m_standardOutput, buffer, outputBytes);
        if (m_timer.elapsed() >= timeoutMilliseconds) {
            const QString output = QString::fromUtf8(m_standardOutput);
            const QString error = QString::fromUtf8(m_standardError);
            closeChannel();
            return {rfm::core::VolumeCommandResult{true, true, false, -1, output, error}, false};
        }
        if (errorBytes > 0 || outputBytes > 0) {
            return {};
        }
        if (ssh_channel_is_eof(m_channel) == 0) {
            return {};
        }
        if (!m_exitStatusReceived && ++m_exitStatusPolls < 100) {
            return {};
        }
        const int exitCode = m_exitStatusReceived ? m_exitCode : -1;
        const QString output = QString::fromUtf8(m_standardOutput);
        const QString error = QString::fromUtf8(m_standardError);
        closeChannel();
        return {rfm::core::VolumeCommandResult{true, false, false, exitCode, output, error}, false};
    }

  private:
    static constexpr qint64 timeoutMilliseconds = 60'000;
    static constexpr qsizetype maximumOutputBytes = 2 * 1024 * 1024;

    static void appendBounded(QByteArray& destination, const char* data, int count)
    {
        if (count <= 0 || destination.size() >= maximumOutputBytes) {
            return;
        }
        destination.append(data,
                           std::min<qsizetype>(count, maximumOutputBytes - destination.size()));
    }

    static void handleExitStatus(ssh_session, ssh_channel, int exitStatus, void* userData)
    {
        auto* const process = static_cast<SshCommandProcess*>(userData);
        process->m_exitStatusReceived = true;
        process->m_exitCode = exitStatus;
    }

    static void handleExitSignal(ssh_session, ssh_channel, const char*, int, const char*,
                                 const char*, void* userData)
    {
        auto* const process = static_cast<SshCommandProcess*>(userData);
        process->m_exitStatusReceived = true;
        process->m_exitCode = -1;
    }

    void closeChannel()
    {
        if (m_channel != nullptr) {
            static_cast<void>(ssh_channel_close(m_channel));
            ssh_channel_free(m_channel);
            m_channel = nullptr;
        }
    }

    ssh_session m_session{nullptr};
    ssh_channel m_channel{nullptr};
    ssh_channel_callbacks_struct m_callbacks{};
    QByteArray m_standardOutput;
    QByteArray m_standardError;
    QElapsedTimer m_timer;
    int m_exitStatusPolls{0};
    int m_exitCode{-1};
    bool m_exitStatusReceived{false};
};

struct SshInteractiveCommandPollResult {
    std::optional<rfm::core::VolumeCommandResult> result;
    bool connectionLost{false};
};

class SshInteractivePolkitProcess final
{
  public:
    explicit SshInteractivePolkitProcess(ssh_session session) : m_session(session) {}
    ~SshInteractivePolkitProcess()
    {
        clearSecret();
        m_parser.clear();
        closeChannel();
    }

    bool start(const QString& command, QByteArray password)
    {
        clearSecret();
        m_password = std::move(password);
        if (m_session == nullptr || ssh_is_connected(m_session) == 0 || command.isEmpty() ||
            m_password.isEmpty()) {
            clearSecret();
            return false;
        }
        m_channel = ssh_channel_new(m_session);
        if (m_channel == nullptr) {
            clearSecret();
            return false;
        }
        m_callbacks = {};
        ssh_callbacks_init(&m_callbacks);
        m_callbacks.userdata = this;
        m_callbacks.channel_exit_status_function = &handleExitStatus;
        m_callbacks.channel_exit_signal_function = &handleExitSignal;
        if (ssh_set_channel_callbacks(m_channel, &m_callbacks) != SSH_OK ||
            ssh_channel_open_session(m_channel) != SSH_OK ||
            ssh_channel_request_pty_size(m_channel, "dumb", 80, 24) != SSH_OK) {
            clearSecret();
            closeChannel();
            return false;
        }
        const QByteArray encoded = command.toUtf8();
        if (ssh_channel_request_exec(m_channel, encoded.constData()) != SSH_OK) {
            clearSecret();
            closeChannel();
            return false;
        }
        m_timer.start();
        return true;
    }

    SshInteractiveCommandPollResult poll()
    {
        if (m_channel == nullptr || m_session == nullptr || ssh_is_connected(m_session) == 0) {
            clearSecret();
            closeChannel();
            return {{}, true};
        }

        char buffer[1024];
        for (int stream : {1, 0}) {
            const int bytes =
                ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), stream);
            if (bytes == SSH_ERROR) {
                const bool lost = ssh_is_connected(m_session) == 0;
                clearSecret();
                closeChannel();
                if (lost) {
                    return {{}, true};
                }
                return {failure(QStringLiteral("The interactive SSH channel failed.")), false};
            }
            if (bytes > 0) {
                const auto event = m_parser.consume(QByteArray(buffer, bytes));
                if (event == rfm::ssh::RemotePolkitPromptEvent::PasswordPrompt) {
                    m_promptSeen = true;
                    m_password.append('\n');
                } else if (event == rfm::ssh::RemotePolkitPromptEvent::AuthenticationFailed) {
                    clearSecret();
                    closeChannel();
                    return {failure(QStringLiteral("Polkit authentication failed.")), false};
                }
            }
        }

        if (m_promptSeen && !m_password.isEmpty()) {
            const qsizetype remaining = m_password.size() - m_passwordOffset;
            const int written =
                ssh_channel_write(m_channel, m_password.constData() + m_passwordOffset,
                                  static_cast<uint32_t>(std::min<qsizetype>(
                                      remaining, std::numeric_limits<int>::max())));
            if (written == SSH_ERROR) {
                const bool lost = ssh_is_connected(m_session) == 0;
                clearSecret();
                closeChannel();
                return lost ? SshInteractiveCommandPollResult{{}, true}
                            : SshInteractiveCommandPollResult{
                                  failure(QStringLiteral("Unable to send Polkit credentials.")),
                                  false};
            }
            if (written > 0) {
                m_passwordOffset += written;
                if (m_passwordOffset >= m_password.size()) {
                    clearSecret();
                    m_parser.passwordSent();
                    m_passwordWasSent = true;
                }
            }
        }

        if (m_timer.elapsed() >= timeoutMilliseconds) {
            const auto timeout = m_parser.timedOut();
            const QString detail = timeout == rfm::ssh::RemotePolkitPromptEvent::TimedOutAfterPrompt
                                       ? QStringLiteral("Polkit authentication timed out.")
                                       : QStringLiteral("Timed out waiting for the Polkit prompt.");
            clearSecret();
            closeChannel();
            return {rfm::core::VolumeCommandResult{true, true, false, -1, {}, detail}, false};
        }
        if (ssh_channel_is_eof(m_channel) == 0) {
            return {};
        }
        if (!m_exitStatusReceived && ++m_exitStatusPolls < 100) {
            return {};
        }

        const int exitCode = m_exitStatusReceived ? m_exitCode : -1;
        QString diagnostic;
        if (exitCode != 0) {
            if (m_parser.volumeBusy()) {
                diagnostic = QStringLiteral("The remote volume is busy.");
            } else if (m_parser.permissionDenied()) {
                diagnostic = QStringLiteral("Polkit permission denied.");
            } else if (m_passwordWasSent && !m_parser.authenticationCompleted()) {
                diagnostic = QStringLiteral("Polkit authentication failed.");
            } else {
                diagnostic = QStringLiteral("The interactive volume command failed.");
            }
        }
        clearSecret();
        closeChannel();
        return {rfm::core::VolumeCommandResult{true, false, false, exitCode, {}, diagnostic},
                false};
    }

  private:
    static constexpr qint64 timeoutMilliseconds = 60'000;

    static rfm::core::VolumeCommandResult failure(QString diagnostic)
    {
        return {true, false, true, -1, {}, std::move(diagnostic)};
    }

    static void handleExitStatus(ssh_session, ssh_channel, int exitStatus, void* userData)
    {
        auto* const process = static_cast<SshInteractivePolkitProcess*>(userData);
        process->m_exitStatusReceived = true;
        process->m_exitCode = exitStatus;
    }

    static void handleExitSignal(ssh_session, ssh_channel, const char*, int, const char*,
                                 const char*, void* userData)
    {
        auto* const process = static_cast<SshInteractivePolkitProcess*>(userData);
        process->m_exitStatusReceived = true;
        process->m_exitCode = -1;
    }

    void clearSecret()
    {
        m_password.fill('\0');
        m_password.clear();
        m_passwordOffset = 0;
    }

    void closeChannel()
    {
        if (m_channel != nullptr) {
            static_cast<void>(ssh_channel_close(m_channel));
            ssh_channel_free(m_channel);
            m_channel = nullptr;
        }
    }

    ssh_session m_session{nullptr};
    ssh_channel m_channel{nullptr};
    ssh_channel_callbacks_struct m_callbacks{};
    rfm::ssh::RemotePolkitPromptParser m_parser;
    QByteArray m_password;
    QElapsedTimer m_timer;
    qsizetype m_passwordOffset{0};
    int m_exitStatusPolls{0};
    int m_exitCode{-1};
    bool m_exitStatusReceived{false};
    bool m_promptSeen{false};
    bool m_passwordWasSent{false};
};

enum class SshVolumeCommandPurpose {
    Capabilities,
    BlockDevices,
    VolumeOperation,
    InteractiveVolumeOperation
};

struct SshVolumeCommandTask {
    SshVolumeCommandPurpose purpose{SshVolumeCommandPurpose::Capabilities};
    QString command;
    quint64 storageRequestId{0};
    rfm::core::VolumeOperationRequest operationRequest;
    QByteArray password;
};

struct AwaitingVolumeAuthentication {
    rfm::core::VolumeOperationRequest request;
    quint64 authenticationToken{0};
};

} // namespace

namespace rfm::ssh
{

class SshSession::Impl final
{
  public:
    ~Impl() { reset(); }

    void reset()
    {
        if (activeVolumeCommand.has_value()) {
            activeVolumeCommand->password.fill('\0');
            activeVolumeCommand->password.clear();
        }
        for (SshVolumeCommandTask& task : volumeCommandQueue) {
            task.password.fill('\0');
            task.password.clear();
        }
        volumeCommandProcess.reset();
        interactiveVolumeCommandProcess.reset();
        activeVolumeCommand.reset();
        volumeCommandQueue.clear();
        activeVolumeDevices.clear();
        pendingStorageRequestId = 0;
        pendingBlockDevices.clear();
        pendingVolumeOperations.clear();
        awaitingVolumeAuthentications.clear();
        volumeCommandStepScheduled = false;
        volumeCapabilityCache.reset();
        if (storageProbeFile != nullptr) {
            sftp_close(storageProbeFile);
            storageProbeFile = nullptr;
        }
        storageProbeData.clear();
        storageProbeRequestId = 0;
        storageProbeStepScheduled = false;
        if (storageScanner != nullptr) {
            storageScanner->cancel();
        }
        storageScanner.reset();
        storageStepScheduled = false;
        if (activeCopyJob != nullptr && !activeCopyJob->isFinished()) {
            static_cast<void>(activeCopyJob->requestCancel());
        }
        activeCopyJob.reset();
        copyBackend.reset();
        activeTransferJob.reset();
        transferBackend.reset();
        transferQueue.clear();
        transferStepScheduled = false;
        copyStepScheduled = false;
        shuttingDown = false;
        disconnecting = false;
        if (sftp != nullptr) {
            sftp_free(sftp);
            sftp = nullptr;
        }
        if (session != nullptr) {
            if (ssh_is_connected(session) != 0) {
                ssh_disconnect(session);
            }
            ssh_free(session);
            session = nullptr;
        }
        password.fill(QChar{'\0'});
        password.clear();
    }

    ssh_session session{nullptr};
    sftp_session sftp{nullptr};
    rfm::core::ConnectionProfile profile;
    QString password;
    bool awaitingHostConfirmation{false};
    // Declaration order is intentional: the job dies before its backend, and
    // both are reset before the SFTP session in reset().
    std::unique_ptr<SftpTransferBackend> transferBackend;
    std::unique_ptr<rfm::core::TransferJob> activeTransferJob;
    std::unique_ptr<SshServerSideCopyBackend> copyBackend;
    std::unique_ptr<rfm::core::ServerSideCopyJob> activeCopyJob;
    std::unique_ptr<rfm::ssh::RemoteStorageScanner> storageScanner;
    std::unique_ptr<SshCommandProcess> volumeCommandProcess;
    std::unique_ptr<SshInteractivePolkitProcess> interactiveVolumeCommandProcess;
    std::optional<SshVolumeCommandTask> activeVolumeCommand;
    QQueue<SshVolumeCommandTask> volumeCommandQueue;
    QSet<QString> activeVolumeDevices;
    QQueue<rfm::core::VolumeOperationRequest> pendingVolumeOperations;
    QHash<quint64, AwaitingVolumeAuthentication> awaitingVolumeAuthentications;
    QList<rfm::core::LinuxBlockDevice> pendingBlockDevices;
    rfm::ssh::RemoteLinuxVolumeCapabilityCache volumeCapabilityCache;
    sftp_file storageProbeFile{nullptr};
    QByteArray storageProbeData;
    quint64 storageProbeRequestId{0};
    quint64 pendingStorageRequestId{0};
    rfm::core::TransferQueue transferQueue;
    bool transferStepScheduled{false};
    bool copyStepScheduled{false};
    bool storageStepScheduled{false};
    bool storageProbeStepScheduled{false};
    bool volumeCommandStepScheduled{false};
    bool shuttingDown{false};
    bool disconnecting{false};
    quint64 nextAuthenticationToken{0};
};

SshSession::SshSession(QObject* parent) : QObject(parent), m_impl(std::make_unique<Impl>()) {}

SshSession::~SshSession() = default;

void SshSession::connectToHost(rfm::core::ConnectionProfile profile, QString password)
{
    m_impl->reset();
    if (!profile.isValid()) {
        fail(tr("Invalid connection settings."));
        return;
    }

    if (profile.allowPasswordFallback) {
        m_impl->password = std::move(password);
    }
    password.fill(QChar{'\0'});
    password.clear();
    m_impl->profile = std::move(profile);
    m_impl->session = ssh_new();
    if (m_impl->session == nullptr) {
        fail(tr("Unable to initialize SSH."));
        return;
    }

    const QByteArray host = m_impl->profile.host.trimmed().toUtf8();
    const QByteArray user = m_impl->profile.username.trimmed().toUtf8();
    unsigned int port = m_impl->profile.port;
    long timeout = 15;
    if (ssh_options_set(m_impl->session, SSH_OPTIONS_HOST, host.constData()) != SSH_OK ||
        ssh_options_set(m_impl->session, SSH_OPTIONS_USER, user.constData()) != SSH_OK ||
        ssh_options_set(m_impl->session, SSH_OPTIONS_PORT, &port) != SSH_OK ||
        ssh_options_set(m_impl->session, SSH_OPTIONS_TIMEOUT, &timeout) != SSH_OK) {
        fail(tr("Unable to configure SSH: %1")
                 .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
        return;
    }

    if (ssh_connect(m_impl->session) != SSH_OK) {
        fail(
            tr("SSH connection failed: %1").arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
        return;
    }

    const auto knownState = ssh_session_is_known_server(m_impl->session);
    if (knownState == SSH_KNOWN_HOSTS_CHANGED || knownState == SSH_KNOWN_HOSTS_OTHER) {
        fail(tr("The server host key does not match known_hosts. Connection refused."));
        return;
    }
    if (knownState == SSH_KNOWN_HOSTS_ERROR) {
        fail(tr("Host key verification failed: %1")
                 .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
        return;
    }
    if (knownState == SSH_KNOWN_HOSTS_UNKNOWN || knownState == SSH_KNOWN_HOSTS_NOT_FOUND) {
        ssh_key key = nullptr;
        unsigned char* hash = nullptr;
        size_t hashLength = 0;
        if (ssh_get_server_publickey(m_impl->session, &key) != SSH_OK ||
            ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLength) != SSH_OK) {
            if (key != nullptr) {
                ssh_key_free(key);
            }
            fail(tr("Unable to read the server host key."));
            return;
        }
        char* const fingerprintText =
            ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLength);
        const QString fingerprint =
            fingerprintText == nullptr ? tr("Unavailable") : QString::fromUtf8(fingerprintText);
        ssh_string_free_char(fingerprintText);
        ssh_clean_pubkey_hash(&hash);
        ssh_key_free(key);
        m_impl->awaitingHostConfirmation = true;
        emit hostKeyConfirmationRequired(m_impl->profile.host, fingerprint);
        return;
    }

    authenticateAndOpen();
}

void SshSession::confirmUnknownHost(bool accepted)
{
    if (!m_impl->awaitingHostConfirmation || m_impl->session == nullptr) {
        return;
    }
    m_impl->awaitingHostConfirmation = false;
    if (!accepted) {
        fail(tr("Connection cancelled: the host key was not trusted."));
        return;
    }
    if (ssh_session_update_known_hosts(m_impl->session) != SSH_OK) {
        fail(tr("Unable to save the host key in known_hosts: %1")
                 .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
        return;
    }
    authenticateAndOpen();
}

void SshSession::authenticateAndOpen()
{
    int auth = ssh_userauth_publickey_auto(m_impl->session, nullptr, nullptr);
    if (auth != SSH_AUTH_SUCCESS && !m_impl->password.isEmpty()) {
        QByteArray passwordBytes = m_impl->password.toUtf8();
        auth = ssh_userauth_password(m_impl->session, nullptr, passwordBytes.constData());
        passwordBytes.fill('\0');
    }
    m_impl->password.fill(QChar{'\0'});
    m_impl->password.clear();
    if (auth != SSH_AUTH_SUCCESS) {
        fail(tr("Authentication failed. Check your SSH agent, keys, or password."));
        return;
    }

    m_impl->sftp = sftp_new(m_impl->session);
    if (m_impl->sftp == nullptr || sftp_init(m_impl->sftp) != SSH_OK) {
        fail(tr("Unable to start the SFTP subsystem: %1")
                 .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
        return;
    }

    char* const canonicalHome = sftp_canonicalize_path(m_impl->sftp, ".");
    if (canonicalHome == nullptr) {
        fail(tr("Unable to resolve the remote home directory: %1")
                 .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
        return;
    }
    const QString initialPath = rfm::core::RemotePath::normalize(QString::fromUtf8(canonicalHome));
    ssh_string_free_char(canonicalHome);
    if (!initialPath.startsWith(QChar{'/'})) {
        fail(tr("The server returned an invalid remote home directory."));
        return;
    }

    const QByteArray encodedInitialPath = initialPath.toUtf8();
    sftp_dir directory = sftp_opendir(m_impl->sftp, encodedInitialPath.constData());
    if (directory == nullptr) {
        fail(tr("Unable to open the remote home directory: %1")
                 .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
        return;
    }

    QList<rfm::core::RemoteEntry> entries;
    while (sftp_attributes attributes = sftp_readdir(m_impl->sftp, directory)) {
        const QString name = QString::fromUtf8(attributes->name);
        if (name != QStringLiteral(".") && name != QStringLiteral("..")) {
            entries.push_back({name, attributes->size,
                               QDateTime::fromSecsSinceEpoch(attributes->mtime),
                               attributes->type == SSH_FILEXFER_TYPE_DIRECTORY,
                               attributes->type == SSH_FILEXFER_TYPE_SYMLINK});
        }
        sftp_attributes_free(attributes);
    }
    const int directoryError =
        sftp_dir_eof(directory) == 0 ? sftp_get_error(m_impl->sftp) : SSH_FX_OK;
    sftp_closedir(directory);
    if (directoryError != SSH_FX_OK) {
        fail(tr("Unable to read the remote home directory."));
        return;
    }
    std::ranges::sort(entries, {}, [](const auto& entry) {
        return std::pair{!entry.directory, entry.name.toCaseFolded()};
    });
    emit connected(initialPath, entries);
}

void SshSession::listDirectory(quint64 requestId, QString path)
{
    if (m_impl->sftp == nullptr) {
        fail(tr("No active SFTP connection."));
        return;
    }
    const QByteArray encodedPath = path.toUtf8();
    sftp_dir directory = sftp_opendir(m_impl->sftp, encodedPath.constData());
    if (directory == nullptr) {
        const int directoryError = sftp_get_error(m_impl->sftp);
        if (isFatalSftpError(directoryError) || m_impl->session == nullptr ||
            ssh_is_connected(m_impl->session) == 0) {
            fail(tr("The SSH connection was lost while opening %1.").arg(path));
        } else {
            emit directoryListingFailed(requestId, path, tr("Unable to open %1.").arg(path));
        }
        return;
    }
    QList<rfm::core::RemoteEntry> entries;
    while (sftp_attributes attributes = sftp_readdir(m_impl->sftp, directory)) {
        const QString name = QString::fromUtf8(attributes->name);
        if (name != QStringLiteral(".") && name != QStringLiteral("..")) {
            entries.push_back({name, attributes->size,
                               QDateTime::fromSecsSinceEpoch(attributes->mtime),
                               attributes->type == SSH_FILEXFER_TYPE_DIRECTORY,
                               attributes->type == SSH_FILEXFER_TYPE_SYMLINK});
        }
        sftp_attributes_free(attributes);
    }
    const int directoryError =
        sftp_dir_eof(directory) == 0 ? sftp_get_error(m_impl->sftp) : SSH_FX_OK;
    sftp_closedir(directory);
    if (directoryError != SSH_FX_OK) {
        if (isFatalSftpError(directoryError) || m_impl->session == nullptr ||
            ssh_is_connected(m_impl->session) == 0) {
            fail(tr("The SSH connection was lost while reading %1.").arg(path));
        } else {
            emit directoryListingFailed(requestId, path, tr("Unable to read %1.").arg(path));
        }
        return;
    }
    std::ranges::sort(entries, {}, [](const auto& entry) {
        return std::pair{!entry.directory, entry.name.toCaseFolded()};
    });
    emit directoryListed(requestId, path, entries);
}

void SshSession::listStorageVolumes(quint64 requestId)
{
    if (m_impl->sftp == nullptr) {
        emit storageVolumeListingFailed(requestId, tr("No active SFTP connection."));
        return;
    }
    cancelStorageProbe();
    cancelStorageScan();
    m_impl->pendingStorageRequestId = requestId;
    m_impl->pendingBlockDevices.clear();
    startPendingRemoteWork();
}

void SshSession::startRemoteStorageScanner(quint64 requestId)
{
    if (requestId == 0 || m_impl->sftp == nullptr) {
        return;
    }
    m_impl->storageScanner = std::make_unique<rfm::ssh::RemoteStorageScanner>(
        std::make_unique<SftpRemoteStorageReader>(m_impl->session, m_impl->sftp), requestId);
    scheduleStorageScanStep();
}

void SshSession::scheduleStorageScanStep()
{
    if (m_impl->storageScanner != nullptr && !m_impl->storageStepScheduled) {
        m_impl->storageStepScheduled = true;
        QMetaObject::invokeMethod(this, &SshSession::processStorageScanStep, Qt::QueuedConnection);
    }
}

void SshSession::processStorageScanStep()
{
    m_impl->storageStepScheduled = false;
    if (m_impl->storageScanner == nullptr) {
        return;
    }
    const quint64 requestId = m_impl->storageScanner->requestId();
    const rfm::ssh::RemoteStorageScanStep result = m_impl->storageScanner->step();
    switch (result.status) {
    case rfm::ssh::RemoteStorageScanStatus::Pending:
        scheduleStorageScanStep();
        return;
    case rfm::ssh::RemoteStorageScanStatus::Completed: {
        QList<rfm::core::StorageVolume> volumes = m_impl->storageScanner->takeVolumes();
        volumes =
            rfm::core::mergeLinuxBlockDevices(std::move(volumes), m_impl->pendingBlockDevices);
        const QByteArray fingerprint = m_impl->storageScanner->mountInfoFingerprint();
        m_impl->pendingBlockDevices.clear();
        m_impl->storageScanner.reset();
        emit storageMountInfoFingerprint(requestId, fingerprint);
        emit storageVolumesListed(requestId, std::move(volumes));
        return;
    }
    case rfm::ssh::RemoteStorageScanStatus::Failed:
        m_impl->pendingBlockDevices.clear();
        m_impl->storageScanner.reset();
        emit storageVolumeListingFailed(requestId, result.error);
        return;
    case rfm::ssh::RemoteStorageScanStatus::ConnectionLost: {
        const QString error = result.error;
        m_impl->pendingBlockDevices.clear();
        m_impl->storageScanner.reset();
        fail(error.isEmpty() ? tr("The SSH connection was lost during storage discovery.") : error);
        return;
    }
    case rfm::ssh::RemoteStorageScanStatus::Cancelled:
        m_impl->storageScanner.reset();
        return;
    }
}

void SshSession::cancelStorageScan()
{
    if (m_impl->storageScanner != nullptr) {
        m_impl->storageScanner->cancel();
        m_impl->storageScanner.reset();
    }
    m_impl->storageStepScheduled = false;
}

void SshSession::operateVolume(rfm::core::VolumeOperationRequest request)
{
    if (m_impl->session == nullptr || m_impl->sftp == nullptr ||
        ssh_is_connected(m_impl->session) == 0) {
        emit volumeOperationFinished(rfm::core::makeVolumeOperationResult(
            request, rfm::core::VolumeOperationError::ConnectionLost,
            QStringLiteral("No active SSH session is available.")));
        return;
    }
    if (!rfm::core::isSafeLinuxDevicePath(request.target.device)) {
        emit volumeOperationFinished(rfm::core::makeVolumeOperationResult(
            request, rfm::core::VolumeOperationError::DeviceNotFound,
            QStringLiteral("The remote volume has no safe Linux device identifier.")));
        return;
    }
    if (m_impl->activeVolumeDevices.contains(request.target.device)) {
        emit volumeOperationFinished(rfm::core::makeVolumeOperationResult(
            request, rfm::core::VolumeOperationError::VolumeBusy,
            QStringLiteral("Another operation is already active for this remote volume.")));
        return;
    }
    m_impl->activeVolumeDevices.insert(request.target.device);
    m_impl->pendingVolumeOperations.enqueue(std::move(request));
    startPendingRemoteWork();
}

void SshSession::authenticateVolume(quint64 operationId, quint64 authenticationToken,
                                    QByteArray password)
{
    auto clearPassword = [&password] {
        password.fill('\0');
        password.clear();
    };
    const auto awaiting = m_impl->awaitingVolumeAuthentications.constFind(operationId);
    if (awaiting == m_impl->awaitingVolumeAuthentications.cend() ||
        awaiting->authenticationToken != authenticationToken || authenticationToken == 0 ||
        password.isEmpty()) {
        clearPassword();
        return;
    }
    if (m_impl->session == nullptr || m_impl->sftp == nullptr ||
        ssh_is_connected(m_impl->session) == 0) {
        const auto request = awaiting->request;
        m_impl->awaitingVolumeAuthentications.erase(awaiting);
        m_impl->activeVolumeDevices.remove(request.target.device);
        clearPassword();
        emit volumeOperationFinished(rfm::core::makeVolumeOperationResult(
            request, rfm::core::VolumeOperationError::ConnectionLost,
            QStringLiteral("The SSH connection was lost before Polkit authentication.")));
        return;
    }

    const rfm::core::VolumeOperationRequest request = awaiting->request;
    m_impl->awaitingVolumeAuthentications.erase(awaiting);
    rfm::core::VolumeOperationResult immediate;
    const auto command = rfm::ssh::RemoteLinuxVolumeService::interactiveOperationCommand(
        request, m_impl->volumeCapabilityCache.value(), &immediate);
    if (!command.has_value()) {
        m_impl->activeVolumeDevices.remove(request.target.device);
        clearPassword();
        emit volumeOperationFinished(immediate);
        return;
    }
    m_impl->volumeCommandQueue.prepend({SshVolumeCommandPurpose::InteractiveVolumeOperation,
                                        *command, 0, request, std::move(password)});
    clearPassword();
    scheduleVolumeCommandStep();
}

void SshSession::cancelVolumeAuthentication(quint64 operationId, quint64 authenticationToken)
{
    const auto awaiting = m_impl->awaitingVolumeAuthentications.find(operationId);
    if (awaiting == m_impl->awaitingVolumeAuthentications.end() || authenticationToken == 0 ||
        awaiting->authenticationToken != authenticationToken) {
        return;
    }
    m_impl->activeVolumeDevices.remove(awaiting->request.target.device);
    m_impl->awaitingVolumeAuthentications.erase(awaiting);
}

void SshSession::startPendingRemoteWork()
{
    if (m_impl->session == nullptr || ssh_is_connected(m_impl->session) == 0) {
        return;
    }
    if (!m_impl->volumeCapabilityCache.value().known) {
        const bool capabilityActive =
            m_impl->activeVolumeCommand.has_value() &&
            m_impl->activeVolumeCommand->purpose == SshVolumeCommandPurpose::Capabilities;
        const bool capabilityQueued =
            std::ranges::any_of(m_impl->volumeCommandQueue, [](const SshVolumeCommandTask& task) {
                return task.purpose == SshVolumeCommandPurpose::Capabilities;
            });
        if (!capabilityActive && !capabilityQueued) {
            m_impl->volumeCommandQueue.prepend(
                {SshVolumeCommandPurpose::Capabilities,
                 rfm::ssh::RemoteLinuxVolumeService::capabilityProbeCommand(),
                 0,
                 {},
                 {}});
        }
        scheduleVolumeCommandStep();
        return;
    }

    if (m_impl->pendingStorageRequestId != 0) {
        const quint64 requestId = std::exchange(m_impl->pendingStorageRequestId, quint64{0});
        if (m_impl->volumeCapabilityCache.value().lsblk) {
            m_impl->volumeCommandQueue.enqueue(
                {SshVolumeCommandPurpose::BlockDevices,
                 rfm::ssh::RemoteLinuxVolumeService::blockDeviceDiscoveryCommand(),
                 requestId,
                 {},
                 {}});
        } else {
            startRemoteStorageScanner(requestId);
        }
    }

    while (!m_impl->pendingVolumeOperations.isEmpty()) {
        const rfm::core::VolumeOperationRequest request = m_impl->pendingVolumeOperations.dequeue();
        rfm::core::VolumeOperationResult immediate;
        const std::optional<QString> command = rfm::ssh::RemoteLinuxVolumeService::operationCommand(
            request, m_impl->volumeCapabilityCache.value(), &immediate);
        if (!command.has_value()) {
            m_impl->activeVolumeDevices.remove(request.target.device);
            emit volumeOperationFinished(immediate);
            continue;
        }
        m_impl->volumeCommandQueue.enqueue(
            {SshVolumeCommandPurpose::VolumeOperation, *command, 0, request, {}});
    }
    scheduleVolumeCommandStep();
}

void SshSession::scheduleVolumeCommandStep()
{
    if (m_impl->activeVolumeCommand == std::nullopt) {
        if (m_impl->volumeCommandQueue.isEmpty()) {
            return;
        }
        m_impl->activeVolumeCommand = m_impl->volumeCommandQueue.dequeue();
        bool started = false;
        if (m_impl->activeVolumeCommand->purpose ==
            SshVolumeCommandPurpose::InteractiveVolumeOperation) {
            m_impl->interactiveVolumeCommandProcess =
                std::make_unique<SshInteractivePolkitProcess>(m_impl->session);
            started = m_impl->interactiveVolumeCommandProcess->start(
                m_impl->activeVolumeCommand->command,
                std::move(m_impl->activeVolumeCommand->password));
            m_impl->activeVolumeCommand->password.fill('\0');
            m_impl->activeVolumeCommand->password.clear();
        } else {
            m_impl->volumeCommandProcess = std::make_unique<SshCommandProcess>(m_impl->session);
            started = m_impl->volumeCommandProcess->start(m_impl->activeVolumeCommand->command);
        }
        if (!started) {
            const SshVolumeCommandTask& task = *m_impl->activeVolumeCommand;
            const bool connectionLost =
                m_impl->session == nullptr || ssh_is_connected(m_impl->session) == 0;
            m_impl->volumeCommandProcess.reset();
            m_impl->interactiveVolumeCommandProcess.reset();
            if (connectionLost) {
                if (task.purpose == SshVolumeCommandPurpose::VolumeOperation ||
                    task.purpose == SshVolumeCommandPurpose::InteractiveVolumeOperation) {
                    emit volumeOperationFinished(rfm::core::makeVolumeOperationResult(
                        task.operationRequest, rfm::core::VolumeOperationError::ConnectionLost,
                        QStringLiteral("The SSH connection was lost before the volume command.")));
                }
                m_impl->activeVolumeCommand.reset();
                fail(tr("The SSH connection was lost before a remote volume command."));
                return;
            }
            if (task.purpose == SshVolumeCommandPurpose::Capabilities) {
                m_impl->volumeCapabilityCache.update({true, false, false, false, false});
            } else if (task.purpose == SshVolumeCommandPurpose::BlockDevices) {
                startRemoteStorageScanner(task.storageRequestId);
            } else {
                m_impl->activeVolumeDevices.remove(task.operationRequest.target.device);
                emit volumeOperationFinished(rfm::core::makeVolumeOperationResult(
                    task.operationRequest, rfm::core::VolumeOperationError::ToolUnavailable,
                    QStringLiteral("The server rejected remote command execution.")));
            }
            m_impl->activeVolumeCommand.reset();
            startPendingRemoteWork();
            scheduleVolumeCommandStep();
            return;
        }
    }
    if (!m_impl->volumeCommandStepScheduled) {
        m_impl->volumeCommandStepScheduled = true;
        QMetaObject::invokeMethod(this, &SshSession::processVolumeCommandStep,
                                  Qt::QueuedConnection);
    }
}

void SshSession::processVolumeCommandStep()
{
    m_impl->volumeCommandStepScheduled = false;
    if (!m_impl->activeVolumeCommand.has_value() ||
        (m_impl->volumeCommandProcess == nullptr &&
         m_impl->interactiveVolumeCommandProcess == nullptr)) {
        scheduleVolumeCommandStep();
        return;
    }
    const bool interactive =
        m_impl->activeVolumeCommand->purpose == SshVolumeCommandPurpose::InteractiveVolumeOperation;
    std::optional<rfm::core::VolumeCommandResult> commandResult;
    bool connectionLost = false;
    if (interactive) {
        const SshInteractiveCommandPollResult poll =
            m_impl->interactiveVolumeCommandProcess->poll();
        commandResult = poll.result;
        connectionLost = poll.connectionLost;
    } else {
        const SshCommandPollResult poll = m_impl->volumeCommandProcess->poll();
        commandResult = poll.result;
        connectionLost = poll.connectionLost;
    }
    if (connectionLost) {
        const SshVolumeCommandTask& task = *m_impl->activeVolumeCommand;
        if (task.purpose == SshVolumeCommandPurpose::VolumeOperation ||
            task.purpose == SshVolumeCommandPurpose::InteractiveVolumeOperation) {
            emit volumeOperationFinished(rfm::core::makeVolumeOperationResult(
                task.operationRequest, rfm::core::VolumeOperationError::ConnectionLost,
                QStringLiteral("The SSH connection was lost during the volume operation.")));
        }
        fail(tr("The SSH connection was lost during a remote volume command."));
        return;
    }
    if (!commandResult.has_value()) {
        scheduleVolumeCommandStep();
        return;
    }

    const SshVolumeCommandTask task = std::move(*m_impl->activeVolumeCommand);
    m_impl->volumeCommandProcess.reset();
    m_impl->interactiveVolumeCommandProcess.reset();
    m_impl->activeVolumeCommand.reset();
    switch (task.purpose) {
    case SshVolumeCommandPurpose::Capabilities:
        m_impl->volumeCapabilityCache.update(
            commandResult->exitCode == 0
                ? rfm::ssh::RemoteLinuxVolumeService::parseCapabilities(
                      commandResult->standardOutput.toUtf8())
                : rfm::ssh::RemoteLinuxVolumeCapabilities{true, false, false, false, false});
        break;
    case SshVolumeCommandPurpose::BlockDevices:
        if (commandResult->exitCode == 0) {
            m_impl->pendingBlockDevices =
                rfm::core::parseLinuxBlockDevices(commandResult->standardOutput.toUtf8());
        } else {
            m_impl->pendingBlockDevices.clear();
        }
        startRemoteStorageScanner(task.storageRequestId);
        break;
    case SshVolumeCommandPurpose::VolumeOperation: {
        auto result = rfm::ssh::RemoteLinuxVolumeService::operationResult(task.operationRequest,
                                                                          *commandResult);
        if (result.error == rfm::core::VolumeOperationError::AuthenticationRequired) {
            quint64 token = ++m_impl->nextAuthenticationToken;
            if (token == 0) {
                token = ++m_impl->nextAuthenticationToken;
            }
            result.authenticationToken = token;
            m_impl->awaitingVolumeAuthentications.insert(task.operationRequest.id,
                                                         {task.operationRequest, token});
        } else {
            m_impl->activeVolumeDevices.remove(task.operationRequest.target.device);
        }
        emit volumeOperationFinished(result);
        break;
    }
    case SshVolumeCommandPurpose::InteractiveVolumeOperation:
        m_impl->activeVolumeDevices.remove(task.operationRequest.target.device);
        emit volumeOperationFinished(rfm::ssh::RemoteLinuxVolumeService::operationResult(
            task.operationRequest, *commandResult));
        break;
    }
    startPendingRemoteWork();
    scheduleVolumeCommandStep();
}

void SshSession::probeStorageMounts(quint64 requestId)
{
    if (m_impl->sftp == nullptr) {
        emit storageMountProbeFailed(requestId, tr("No active SFTP connection."));
        return;
    }
    if (m_impl->storageScanner != nullptr || m_impl->storageProbeRequestId != 0) {
        return;
    }
    const QByteArray path = QByteArrayLiteral("/proc/self/mountinfo");
    m_impl->storageProbeFile = sftp_open(m_impl->sftp, path.constData(), O_RDONLY, 0);
    if (m_impl->storageProbeFile == nullptr) {
        emit storageMountProbeFailed(requestId, tr("Unable to open remote mount information."));
        return;
    }
    m_impl->storageProbeRequestId = requestId;
    m_impl->storageProbeData.clear();
    scheduleStorageProbeStep();
}

void SshSession::scheduleStorageProbeStep()
{
    if (m_impl->storageProbeRequestId != 0 && !m_impl->storageProbeStepScheduled) {
        m_impl->storageProbeStepScheduled = true;
        QMetaObject::invokeMethod(this, &SshSession::processStorageProbeStep, Qt::QueuedConnection);
    }
}

void SshSession::processStorageProbeStep()
{
    m_impl->storageProbeStepScheduled = false;
    if (m_impl->storageProbeRequestId == 0 || m_impl->storageProbeFile == nullptr) {
        return;
    }
    constexpr qsizetype maximumMountInfoBytes = 1024 * 1024;
    char buffer[4096];
    const ssize_t count = sftp_read(m_impl->storageProbeFile, buffer, sizeof(buffer));
    if (count < 0) {
        const quint64 requestId = m_impl->storageProbeRequestId;
        const rfm::ssh::RemoteStorageError error = storageError(m_impl->session, m_impl->sftp);
        cancelStorageProbe();
        if (error == rfm::ssh::RemoteStorageError::ConnectionLost) {
            fail(tr("The SSH connection was lost while probing storage."));
        } else {
            emit storageMountProbeFailed(requestId, tr("Unable to read remote mount information."));
        }
        return;
    }
    if (count == 0) {
        const quint64 requestId = m_impl->storageProbeRequestId;
        const QByteArray fingerprint =
            QCryptographicHash::hash(m_impl->storageProbeData, QCryptographicHash::Sha256);
        cancelStorageProbe();
        emit storageMountsProbed(requestId, fingerprint);
        return;
    }
    if (m_impl->storageProbeData.size() > maximumMountInfoBytes - count) {
        const quint64 requestId = m_impl->storageProbeRequestId;
        cancelStorageProbe();
        emit storageMountProbeFailed(requestId,
                                     tr("Remote mount information exceeds the safety limit."));
        return;
    }
    m_impl->storageProbeData.append(buffer, static_cast<qsizetype>(count));
    scheduleStorageProbeStep();
}

void SshSession::cancelStorageProbe()
{
    if (m_impl->storageProbeFile != nullptr) {
        sftp_close(m_impl->storageProbeFile);
        m_impl->storageProbeFile = nullptr;
    }
    m_impl->storageProbeData.clear();
    m_impl->storageProbeRequestId = 0;
    m_impl->storageProbeStepScheduled = false;
}

void SshSession::createDirectory(quint64 id, QString parent, QString name)
{
    if (m_impl->sftp == nullptr) {
        emit failed(tr("Aucune connexion SFTP active."));
        return;
    }
    SftpBackend backend(m_impl->sftp);
    rfm::core::RemoteFileOperations operations(backend);
    emit operationFinished(operations.createDirectory(id, parent, name));
}

void SshSession::renameEntry(quint64 id, QString source, QString newName)
{
    if (m_impl->sftp == nullptr) {
        emit failed(tr("Aucune connexion SFTP active."));
        return;
    }
    SftpBackend backend(m_impl->sftp);
    rfm::core::RemoteFileOperations operations(backend);
    emit operationFinished(operations.rename(id, source, newName));
}

void SshSession::moveEntries(quint64 id, QList<rfm::core::RemoteSelection> sources,
                             QString destinationDirectory)
{
    if (m_impl->sftp == nullptr) {
        emit failed(tr("Aucune connexion SFTP active."));
        return;
    }
    SftpBackend backend(m_impl->sftp);
    rfm::core::RemoteFileOperations operations(backend);
    emit operationFinished(operations.move(id, sources, destinationDirectory));
}

void SshSession::copyEntries(quint64 id, QList<rfm::core::RemoteSelection> sources,
                             QString destinationDirectory)
{
    if (m_impl->sftp == nullptr) {
        emit failed(tr("Aucune connexion SFTP active."));
        return;
    }
    if (m_impl->activeCopyJob != nullptr) {
        rfm::core::RemoteOperationResult rejected{id, rfm::core::RemoteOperationKind::Copy, {}};
        for (const rfm::core::RemoteSelection& source : std::as_const(sources)) {
            rejected.items.push_back(
                {source.path, {}, false, tr("Another server-side copy is already active.")});
        }
        emit operationFinished(rejected);
        return;
    }
    m_impl->copyBackend = std::make_unique<SshServerSideCopyBackend>(m_impl->session, m_impl->sftp);
    m_impl->activeCopyJob = std::make_unique<rfm::core::ServerSideCopyJob>(
        *m_impl->copyBackend, id, std::move(sources), std::move(destinationDirectory));
    emit operationUpdated(m_impl->activeCopyJob->progress());
    scheduleCopyStep();
}

void SshSession::removeEntries(quint64 id, QList<rfm::core::RemoteSelection> sources,
                               bool recursive)
{
    if (m_impl->sftp == nullptr) {
        emit failed(tr("Aucune connexion SFTP active."));
        return;
    }
    SftpBackend backend(m_impl->sftp);
    rfm::core::RemoteFileOperations operations(backend);
    emit operationFinished(operations.remove(id, sources, recursive));
}

void SshSession::enqueueTransfer(rfm::core::TransferRequest request)
{
    if (m_impl->sftp == nullptr || request.id == 0 || request.source.isEmpty() ||
        request.destination.isEmpty() || request.source.contains(QChar{'\0'}) ||
        request.destination.contains(QChar{'\0'})) {
        emit transferRejected(request.id,
                              tr("Invalid transfer request or no active SFTP connection."));
        return;
    }
    if ((m_impl->activeTransferJob != nullptr &&
         m_impl->activeTransferJob->progress().id == request.id) ||
        !m_impl->transferQueue.enqueue(request)) {
        emit transferRejected(request.id, tr("A transfer with this identifier already exists."));
        return;
    }
    emit transferUpdated({request.id,
                          rfm::core::TransferState::Queued,
                          request.source,
                          request.destination,
                          0,
                          0,
                          0,
                          {},
                          0,
                          0,
                          {},
                          request.direction,
                          request.directory});
    scheduleTransferStep();
}

void SshSession::pauseTransfer(quint64 id)
{
    if (m_impl->activeTransferJob != nullptr && m_impl->activeTransferJob->progress().id == id &&
        m_impl->activeTransferJob->requestPause()) {
        emit transferUpdated(m_impl->activeTransferJob->progress());
        return;
    }
    emit transferRejected(id, tr("Only the active transfer can be paused."));
}

void SshSession::resumeTransfer(quint64 id)
{
    if (m_impl->activeTransferJob != nullptr && m_impl->activeTransferJob->progress().id == id &&
        m_impl->activeTransferJob->resume()) {
        emit transferUpdated(m_impl->activeTransferJob->progress());
        scheduleTransferStep();
        return;
    }
    emit transferRejected(id, tr("Only a paused active transfer can be resumed."));
}

void SshSession::cancelTransfer(quint64 id)
{
    if (m_impl->activeTransferJob != nullptr && m_impl->activeTransferJob->progress().id == id) {
        if (m_impl->activeTransferJob->requestCancel()) {
            emit transferUpdated(m_impl->activeTransferJob->progress());
            scheduleTransferStep();
        }
        return;
    }

    rfm::core::TransferRequest cancelled;
    if (m_impl->transferQueue.cancel(id, cancelled)) {
        emit transferUpdated({cancelled.id,
                              rfm::core::TransferState::Cancelled,
                              cancelled.source,
                              cancelled.destination,
                              0,
                              0,
                              0,
                              {},
                              0,
                              0,
                              {},
                              cancelled.direction,
                              cancelled.directory});
        return;
    }
    emit transferRejected(id, tr("The transfer was not found or is already terminal."));
}

void SshSession::cancelRemoteOperation(quint64 id)
{
    if (m_impl->activeCopyJob == nullptr || m_impl->activeCopyJob->progress().id != id ||
        !m_impl->activeCopyJob->requestCancel()) {
        return;
    }
    emit operationUpdated(m_impl->activeCopyJob->progress());
    scheduleCopyStep();
}

void SshSession::shutdownTransfers()
{
    cancelStorageProbe();
    cancelStorageScan();
    m_impl->transferQueue.clear();
    m_impl->shuttingDown = true;
    if (m_impl->activeTransferJob != nullptr) {
        if (!m_impl->activeTransferJob->isFinished()) {
            static_cast<void>(m_impl->activeTransferJob->requestCancel());
        }
        scheduleTransferStep();
    }
    if (m_impl->activeCopyJob != nullptr) {
        if (!m_impl->activeCopyJob->isFinished()) {
            static_cast<void>(m_impl->activeCopyJob->requestCancel());
        }
        scheduleCopyStep();
    }
    completeShutdownIfReady();
}

void SshSession::scheduleTransferStep()
{
    if (!m_impl->transferStepScheduled) {
        m_impl->transferStepScheduled = true;
        QMetaObject::invokeMethod(this, &SshSession::processTransferStep, Qt::QueuedConnection);
    }
}

void SshSession::processTransferStep()
{
    m_impl->transferStepScheduled = false;
    if (m_impl->activeTransferJob != nullptr) {
        if (m_impl->activeTransferJob->isPaused()) {
            return;
        }
        m_impl->activeTransferJob->step();
        emit transferUpdated(m_impl->activeTransferJob->progress());
        if (m_impl->activeTransferJob->isFinished()) {
            m_impl->activeTransferJob.reset();
            m_impl->transferBackend.reset();
            const bool shuttingDown = m_impl->shuttingDown;
            completeShutdownIfReady();
            if (shuttingDown) {
                return;
            }
            if (!m_impl->transferQueue.isEmpty()) {
                scheduleTransferStep();
            }
        } else {
            scheduleTransferStep();
        }
        return;
    }
    const std::optional<rfm::core::TransferRequest> next = m_impl->transferQueue.takeNext();
    if (!next.has_value()) {
        return;
    }

    m_impl->transferBackend = std::make_unique<SftpTransferBackend>(m_impl->sftp);
    if (next->directory) {
        m_impl->activeTransferJob =
            std::make_unique<rfm::core::TransferDirectoryJob>(*m_impl->transferBackend, *next);
    } else {
        m_impl->activeTransferJob =
            std::make_unique<rfm::core::TransferFileJob>(*m_impl->transferBackend, *next);
    }
    scheduleTransferStep();
}

void SshSession::scheduleCopyStep()
{
    if (!m_impl->copyStepScheduled) {
        m_impl->copyStepScheduled = true;
        QTimer::singleShot(10, this, &SshSession::processCopyStep);
    }
}

void SshSession::processCopyStep()
{
    m_impl->copyStepScheduled = false;
    if (m_impl->activeCopyJob == nullptr) {
        completeShutdownIfReady();
        return;
    }
    if (!m_impl->activeCopyJob->isFinished()) {
        m_impl->activeCopyJob->step();
        emit operationUpdated(m_impl->activeCopyJob->progress());
    }
    if (m_impl->activeCopyJob->isFinished()) {
        const rfm::core::RemoteOperationResult result = m_impl->activeCopyJob->result();
        m_impl->activeCopyJob.reset();
        m_impl->copyBackend.reset();
        emit operationFinished(result);
        completeShutdownIfReady();
        return;
    }
    scheduleCopyStep();
}

void SshSession::completeShutdownIfReady()
{
    if ((!m_impl->shuttingDown && !m_impl->disconnecting) || m_impl->activeTransferJob != nullptr ||
        m_impl->activeCopyJob != nullptr) {
        return;
    }
    const bool emitTransfersShutdown = m_impl->shuttingDown;
    const bool emitDisconnected = m_impl->disconnecting;
    m_impl->reset();
    if (emitTransfersShutdown) {
        emit transfersShutdown();
    }
    if (emitDisconnected) {
        emit disconnected();
    }
}

void SshSession::disconnectFromHost()
{
    cancelStorageProbe();
    cancelStorageScan();
    m_impl->transferQueue.clear();
    m_impl->disconnecting = true;
    if (m_impl->activeTransferJob != nullptr && !m_impl->activeTransferJob->isFinished()) {
        static_cast<void>(m_impl->activeTransferJob->requestCancel());
        scheduleTransferStep();
    }
    if (m_impl->activeCopyJob != nullptr && !m_impl->activeCopyJob->isFinished()) {
        static_cast<void>(m_impl->activeCopyJob->requestCancel());
        scheduleCopyStep();
    }
    completeShutdownIfReady();
}

void SshSession::fail(const QString& message)
{
    m_impl->reset();
    emit failed(message);
}

} // namespace rfm::ssh
