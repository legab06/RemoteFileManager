#include "remotefilemanager/ssh/SshSession.hpp"

#include "SftpTransferBackend.hpp"
#include "remotefilemanager/core/TransferDirectoryJob.hpp"
#include "remotefilemanager/core/TransferFileJob.hpp"
#include "remotefilemanager/core/TransferJob.hpp"
#include "remotefilemanager/core/TransferQueue.hpp"
#include "remotefilemanager/ssh/RemoteCopyCommand.hpp"

#include <libssh/libssh.h>
#include <libssh/libssh_version.h>
#include <libssh/sftp.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QMetaObject>

#include <algorithm>

namespace {

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

class SftpBackend final : public rfm::core::RemoteFileBackend {
public:
    SftpBackend(ssh_session session, sftp_session sftp)
        : m_session(session)
        , m_sftp(sftp)
    {
    }

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

    rfm::core::RemoteBackendResult rename(
        const QString& source, const QString& destination) override
    {
        const QByteArray encodedSource = source.toUtf8();
        const QByteArray encodedDestination = destination.toUtf8();
        if (sftp_rename(
                m_sftp, encodedSource.constData(), encodedDestination.constData())
            == SSH_OK) {
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

    rfm::core::RemoteBackendResult copyOnServer(
        const QString& source, const QString& destination, bool recursive) override
    {
        const QString command = rfm::ssh::RemoteCopyCommand::build(source, destination, recursive);
        if (command.isEmpty()) {
            return {rfm::core::RemoteBackendError::InvalidPath, {}};
        }
        ssh_channel channel = ssh_channel_new(m_session);
        if (channel == nullptr) {
            return {rfm::core::RemoteBackendError::Failure,
                    QCoreApplication::translate("SftpBackend", "Unable to open an SSH channel.")};
        }
        const auto closeChannel = [&channel] {
            ssh_channel_close(channel);
            ssh_channel_free(channel);
        };
        if (ssh_channel_open_session(channel) != SSH_OK) {
            closeChannel();
            return {rfm::core::RemoteBackendError::Unsupported,
                    QCoreApplication::translate(
                        "SftpBackend", "The server rejected remote SSH commands.")};
        }
        const QByteArray encodedCommand = command.toUtf8();
        if (ssh_channel_request_exec(channel, encodedCommand.constData()) != SSH_OK) {
            closeChannel();
            return {rfm::core::RemoteBackendError::Unsupported,
                    QCoreApplication::translate(
                        "SftpBackend", "Remote copy is not supported by this server.")};
        }

        QByteArray errorOutput;
        char buffer[512];
        int bytesRead = 0;
        do {
            bytesRead = ssh_channel_read(channel, buffer, sizeof(buffer), 1);
            if (bytesRead > 0 && errorOutput.size() < 2048) {
                const int remaining = 2048 - static_cast<int>(errorOutput.size());
                errorOutput.append(buffer, std::min(bytesRead, remaining));
            }
        } while (bytesRead > 0);
        uint32_t exitCode = UINT32_MAX;
#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0, 11, 0)
        const int exitState = ssh_channel_get_exit_state(channel, &exitCode, nullptr, nullptr);
#else
        const int legacyExitCode = ssh_channel_get_exit_status(channel);
        const int exitState = legacyExitCode >= 0 ? SSH_OK : SSH_ERROR;
        if (legacyExitCode >= 0) {
            exitCode = static_cast<uint32_t>(legacyExitCode);
        }
#endif
        closeChannel();
        if (exitState == SSH_OK && exitCode == 0) {
            return {};
        }
        const QString detail = QString::fromUtf8(errorOutput).trimmed();
        if (exitCode == 126 || exitCode == 127) {
            return {rfm::core::RemoteBackendError::Unsupported,
                    QCoreApplication::translate(
                        "SftpBackend", "The 'cp' command is not available on the server.")};
        }
        return {rfm::core::RemoteBackendError::Failure,
                detail.isEmpty()
                    ? QCoreApplication::translate("SftpBackend", "Remote copy failed.")
                    : detail};
    }

private:
    ssh_session m_session;
    sftp_session m_sftp;
};

}  // namespace

namespace rfm::ssh {

class SshSession::Impl final {
public:
    ~Impl()
    {
        reset();
    }

    void reset()
    {
        activeTransferJob.reset();
        transferBackend.reset();
        transferQueue.clear();
        transferStepScheduled = false;
        shuttingDown = false;
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
    rfm::core::TransferQueue transferQueue;
    bool transferStepScheduled{false};
    bool shuttingDown{false};
};

SshSession::SshSession(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
}

SshSession::~SshSession() = default;

void SshSession::connectToHost(rfm::core::ConnectionProfile profile, QString password)
{
    m_impl->reset();
    if (!profile.isValid()) {
        fail(tr("Invalid connection settings."));
        return;
    }

    m_impl->profile = std::move(profile);
    m_impl->password = std::move(password);
    m_impl->session = ssh_new();
    if (m_impl->session == nullptr) {
        fail(tr("Unable to initialize SSH."));
        return;
    }

    const QByteArray host = m_impl->profile.host.trimmed().toUtf8();
    const QByteArray user = m_impl->profile.username.trimmed().toUtf8();
    unsigned int port = m_impl->profile.port;
    long timeout = 15;
    if (ssh_options_set(m_impl->session, SSH_OPTIONS_HOST, host.constData()) != SSH_OK
        || ssh_options_set(m_impl->session, SSH_OPTIONS_USER, user.constData()) != SSH_OK
        || ssh_options_set(m_impl->session, SSH_OPTIONS_PORT, &port) != SSH_OK
        || ssh_options_set(m_impl->session, SSH_OPTIONS_TIMEOUT, &timeout) != SSH_OK) {
        fail(tr("Unable to configure SSH: %1")
                 .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
        return;
    }

    if (ssh_connect(m_impl->session) != SSH_OK) {
        fail(tr("SSH connection failed: %1")
                 .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
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
        if (ssh_get_server_publickey(m_impl->session, &key) != SSH_OK
            || ssh_get_publickey_hash(
                   key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLength)
                != SSH_OK) {
            if (key != nullptr) {
                ssh_key_free(key);
            }
            fail(tr("Unable to read the server host key."));
            return;
        }
        char* const fingerprintText =
            ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLength);
        const QString fingerprint = fingerprintText == nullptr
            ? tr("Unavailable")
            : QString::fromUtf8(fingerprintText);
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

    sftp_attributes home = sftp_stat(m_impl->sftp, ".");
    const QString initialPath = QStringLiteral(".");
    if (home != nullptr) {
        sftp_attributes_free(home);
    }

    sftp_dir directory = sftp_opendir(m_impl->sftp, ".");
    if (directory == nullptr) {
        fail(tr("Unable to open the remote home directory: %1")
                 .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
        return;
    }

    QList<rfm::core::RemoteEntry> entries;
    while (sftp_attributes attributes = sftp_readdir(m_impl->sftp, directory)) {
        const QString name = QString::fromUtf8(attributes->name);
        if (name != QStringLiteral(".") && name != QStringLiteral("..")) {
            entries.push_back({name,
                               attributes->size,
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

void SshSession::listDirectory(QString path)
{
    if (m_impl->sftp == nullptr) {
        fail(tr("No active SFTP connection."));
        return;
    }
    const QByteArray encodedPath = path.toUtf8();
    sftp_dir directory = sftp_opendir(m_impl->sftp, encodedPath.constData());
    if (directory == nullptr) {
        fail(tr("Unable to open %1.").arg(path));
        return;
    }
    QList<rfm::core::RemoteEntry> entries;
    while (sftp_attributes attributes = sftp_readdir(m_impl->sftp, directory)) {
        const QString name = QString::fromUtf8(attributes->name);
        if (name != QStringLiteral(".") && name != QStringLiteral("..")) {
            entries.push_back({name,
                               attributes->size,
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
        fail(tr("Unable to read %1.").arg(path));
        return;
    }
    std::ranges::sort(entries, {}, [](const auto& entry) {
        return std::pair{!entry.directory, entry.name.toCaseFolded()};
    });
    emit directoryListed(path, entries);
}

void SshSession::createDirectory(quint64 id, QString parent, QString name)
{
    if (m_impl->sftp == nullptr) {
        emit failed(tr("Aucune connexion SFTP active."));
        return;
    }
    SftpBackend backend(m_impl->session, m_impl->sftp);
    rfm::core::RemoteFileOperations operations(backend);
    emit operationFinished(operations.createDirectory(id, parent, name));
}

void SshSession::renameEntry(quint64 id, QString source, QString newName)
{
    if (m_impl->sftp == nullptr) {
        emit failed(tr("Aucune connexion SFTP active."));
        return;
    }
    SftpBackend backend(m_impl->session, m_impl->sftp);
    rfm::core::RemoteFileOperations operations(backend);
    emit operationFinished(operations.rename(id, source, newName));
}

void SshSession::moveEntries(
    quint64 id, QList<rfm::core::RemoteSelection> sources, QString destinationDirectory)
{
    if (m_impl->sftp == nullptr) {
        emit failed(tr("Aucune connexion SFTP active."));
        return;
    }
    SftpBackend backend(m_impl->session, m_impl->sftp);
    rfm::core::RemoteFileOperations operations(backend);
    emit operationFinished(operations.move(id, sources, destinationDirectory));
}

void SshSession::copyEntries(
    quint64 id, QList<rfm::core::RemoteSelection> sources, QString destinationDirectory)
{
    if (m_impl->sftp == nullptr) {
        emit failed(tr("Aucune connexion SFTP active."));
        return;
    }
    SftpBackend backend(m_impl->session, m_impl->sftp);
    rfm::core::RemoteFileOperations operations(backend);
    emit operationFinished(operations.copy(id, sources, destinationDirectory));
}

void SshSession::removeEntries(
    quint64 id, QList<rfm::core::RemoteSelection> sources, bool recursive)
{
    if (m_impl->sftp == nullptr) {
        emit failed(tr("Aucune connexion SFTP active."));
        return;
    }
    SftpBackend backend(m_impl->session, m_impl->sftp);
    rfm::core::RemoteFileOperations operations(backend);
    emit operationFinished(operations.remove(id, sources, recursive));
}

void SshSession::enqueueTransfer(rfm::core::TransferRequest request)
{
    request.source = request.source.trimmed();
    request.destination = request.destination.trimmed();
    if (m_impl->sftp == nullptr || request.id == 0 || request.source.isEmpty() ||
        request.destination.isEmpty()) {
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

void SshSession::shutdownTransfers()
{
    m_impl->transferQueue.clear();
    if (m_impl->activeTransferJob == nullptr || m_impl->activeTransferJob->isFinished()) {
        m_impl->reset();
        emit transfersShutdown();
        return;
    }
    m_impl->shuttingDown = true;
    static_cast<void>(m_impl->activeTransferJob->requestCancel());
    scheduleTransferStep();
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
            if (m_impl->shuttingDown) {
                m_impl->reset();
                emit transfersShutdown();
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

void SshSession::disconnectFromHost()
{
    m_impl->reset();
    emit disconnected();
}

void SshSession::fail(const QString& message)
{
    m_impl->reset();
    emit failed(message);
}

}  // namespace rfm::ssh
