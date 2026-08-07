#include "remotefilemanager/ssh/SshSession.hpp"

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <QByteArray>
#include <QDateTime>

#include <algorithm>

namespace rfm::ssh {

class SshSession::Impl final {
public:
    ~Impl()
    {
        reset();
    }

    void reset()
    {
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
        fail(tr("Unable to configure SSH: %1").arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
        return;
    }

    if (ssh_connect(m_impl->session) != SSH_OK) {
        fail(tr("SSH connection failed: %1").arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
        return;
    }

    const auto knownState = ssh_session_is_known_server(m_impl->session);
    if (knownState == SSH_KNOWN_HOSTS_CHANGED || knownState == SSH_KNOWN_HOSTS_OTHER) {
        fail(tr("The server host key does not match known_hosts. Connection refused."));
        return;
    }
    if (knownState == SSH_KNOWN_HOSTS_ERROR) {
        fail(tr("Host key verification failed: %1").arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
        return;
    }
    if (knownState == SSH_KNOWN_HOSTS_UNKNOWN || knownState == SSH_KNOWN_HOSTS_NOT_FOUND) {
        ssh_key key = nullptr;
        unsigned char* hash = nullptr;
        size_t hashLength = 0;
        if (ssh_get_server_publickey(m_impl->session, &key) != SSH_OK
            || ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLength) != SSH_OK) {
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
    const int directoryError = sftp_dir_eof(directory) == 0 ? sftp_get_error(m_impl->sftp) : SSH_FX_OK;
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
            entries.push_back({name, attributes->size, QDateTime::fromSecsSinceEpoch(attributes->mtime),
                               attributes->type == SSH_FILEXFER_TYPE_DIRECTORY,
                               attributes->type == SSH_FILEXFER_TYPE_SYMLINK});
        }
        sftp_attributes_free(attributes);
    }
    const int directoryError = sftp_dir_eof(directory) == 0 ? sftp_get_error(m_impl->sftp) : SSH_FX_OK;
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
