#include "remotefilemanager/ssh/SshSession.hpp"

#include "RemoteDelete.hpp"
#include "SftpTransferBackend.hpp"
#include "SshTransportHealth.hpp"
#include "remotefilemanager/core/RemoteMoveSafety.hpp"
#include "remotefilemanager/core/RemotePath.hpp"
#include "remotefilemanager/core/ServerSideCopyJob.hpp"
#include "remotefilemanager/core/Storage.hpp"
#include "remotefilemanager/core/TransferDirectoryJob.hpp"
#include "remotefilemanager/core/TransferFileJob.hpp"
#include "remotefilemanager/core/TransferJob.hpp"
#include "remotefilemanager/ssh/RemoteCopyCapabilityProbe.hpp"
#include "remotefilemanager/ssh/RemoteCopyCommand.hpp"
#include "remotefilemanager/ssh/RemoteStorageCapabilityProbe.hpp"
#include "remotefilemanager/ssh/RemoteStorageScanner.hpp"
#include "remotefilemanager/ssh/RemoteVolumeService.hpp"
#include "remotefilemanager/ssh/SshAuthenticationPolicy.hpp"

#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QHash>
#include <QMetaObject>
#include <QQueue>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <utility>

namespace
{

QEvent::Type volumeAuthenticationEventType()
{
    static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
    return type;
}

class VolumeAuthenticationEvent final : public QEvent
{
  public:
    VolumeAuthenticationEvent(quint64 operationId, quint64 authenticationToken,
                              rfm::core::SecurePassword password)
        : QEvent(volumeAuthenticationEventType()), operationId(operationId),
          authenticationToken(authenticationToken), password(std::move(password))
    {}

    quint64 operationId{0};
    quint64 authenticationToken{0};
    rfm::core::SecurePassword password;
};

QEvent::Type passwordAuthenticationEventType()
{
    static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
    return type;
}

class PasswordAuthenticationEvent final : public QEvent
{
  public:
    explicit PasswordAuthenticationEvent(rfm::core::SecurePassword password)
        : QEvent(passwordAuthenticationEventType()), password(std::move(password))
    {}

    rfm::core::SecurePassword password;
};

rfm::ssh::AuthenticationResult authenticationResult(int result)
{
    switch (result) {
    case SSH_AUTH_SUCCESS:
        return rfm::ssh::AuthenticationResult::Success;
    case SSH_AUTH_DENIED:
        return rfm::ssh::AuthenticationResult::Denied;
    case SSH_AUTH_PARTIAL:
        return rfm::ssh::AuthenticationResult::Partial;
    default:
        return rfm::ssh::AuthenticationResult::Error;
    }
}

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

QList<rfm::core::SftpExtensionCapability> announcedSftpExtensions(sftp_session sftp)
{
    QList<rfm::core::SftpExtensionCapability> extensions;
    const unsigned int count = sftp_extensions_get_count(sftp);
    extensions.reserve(static_cast<qsizetype>(count));
    for (unsigned int index = 0; index < count; ++index) {
        const char* const name = sftp_extensions_get_name(sftp, index);
        if (name == nullptr) {
            continue;
        }
        const char* const data = sftp_extensions_get_data(sftp, index);
        extensions.push_back(
            {QString::fromUtf8(name), data == nullptr ? QString{} : QString::fromUtf8(data)});
    }
    return extensions;
}

bool isFatalSftpError(int sftpError)
{
    return sftpError == SSH_FX_NO_CONNECTION || sftpError == SSH_FX_CONNECTION_LOST;
}

QDateTime remoteModificationTime(const sftp_attributes attributes)
{
    if ((attributes->flags & SSH_FILEXFER_ATTR_MODIFYTIME) != 0U) {
        if (attributes->mtime64 > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
            return {};
        }
        return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(attributes->mtime64));
    }
    if ((attributes->flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0U) {
        return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(attributes->mtime));
    }
    return {};
}

std::optional<quint64> remoteFileSystemId(sftp_session sftp, const QString& path)
{
    const QByteArray encoded = path.toUtf8();
    sftp_statvfs_t attributes = sftp_statvfs(sftp, encoded.constData());
    if (attributes == nullptr) {
        return std::nullopt;
    }
    const quint64 id = attributes->f_fsid;
    sftp_statvfs_free(attributes);
    return id == 0 ? std::nullopt : std::optional<quint64>{id};
}

enum class RemoteMountInfoState { Unavailable, Available, Invalid };

struct RemoteMountInfoResult {
    RemoteMountInfoState state{RemoteMountInfoState::Unavailable};
    QByteArray contents;
};

RemoteMountInfoResult remoteMountInfo(sftp_session sftp)
{
    constexpr qsizetype maximumBytes = 1024 * 1024;
    const QByteArray path = QByteArrayLiteral("/proc/self/mountinfo");
    sftp_file file = sftp_open(sftp, path.constData(), O_RDONLY, 0);
    if (file == nullptr) {
        const int error = sftp_get_error(sftp);
        return {error == SSH_FX_NO_SUCH_FILE || error == SSH_FX_NO_SUCH_PATH
                    ? RemoteMountInfoState::Unavailable
                    : RemoteMountInfoState::Invalid,
                {}};
    }
    QByteArray contents;
    char buffer[4096];
    while (contents.size() <= maximumBytes) {
        const ssize_t count = sftp_read(file, buffer, sizeof(buffer));
        if (count < 0) {
            sftp_close(file);
            return {RemoteMountInfoState::Invalid, {}};
        }
        if (count == 0) {
            sftp_close(file);
            return {RemoteMountInfoState::Available, std::move(contents)};
        }
        contents.append(buffer, static_cast<qsizetype>(count));
    }
    sftp_close(file);
    return {RemoteMountInfoState::Invalid, {}};
}

std::optional<QString> canonicalRemoteEntryPath(sftp_session sftp, const QString& path)
{
    const QString container = rfm::core::remoteMoveFileSystemContainer(path);
    const QString name = rfm::core::RemotePath::fileName(path);
    if (container.isEmpty() || name.isEmpty()) {
        return std::nullopt;
    }
    const QByteArray encodedContainer = container.toUtf8();
    char* const canonical = sftp_canonicalize_path(sftp, encodedContainer.constData());
    if (canonical == nullptr) {
        return std::nullopt;
    }
    const QString canonicalContainer = QString::fromUtf8(canonical);
    ssh_string_free_char(canonical);
    const QString entry = rfm::core::RemotePath::join(canonicalContainer, name);
    return entry.isEmpty() ? std::nullopt : std::optional<QString>{entry};
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
        const int error = sftp_get_error(m_sftp);
        if (error == SSH_FX_FAILURE) {
            // SFTP v3 reports both EXDEV and EBUSY as SSH_FX_FAILURE. Compare
            // the directories containing both entries and reject mount points
            // or incomplete evidence before enabling copy-then-delete.
            const QString sourceContainer = rfm::core::remoteMoveFileSystemContainer(source);
            const QString destinationContainer =
                rfm::core::remoteMoveFileSystemContainer(destination);
            const RemoteMountInfoResult mountInfo = remoteMountInfo(m_sftp);
            const std::optional<QString> canonicalSource = canonicalRemoteEntryPath(m_sftp, source);
            const rfm::core::RemoteMoveFallbackEvidence evidence{
                remoteFileSystemId(m_sftp, sourceContainer),
                remoteFileSystemId(m_sftp, destinationContainer),
                mountInfo.state == RemoteMountInfoState::Available && canonicalSource.has_value()
                    ? rfm::core::linuxMountPointState(mountInfo.contents, *canonicalSource)
                    : rfm::core::RemoteMountPointState::Unknown};
            if (rfm::core::allowsRemoteMoveFallback(evidence)) {
                return {rfm::core::RemoteBackendError::CrossDevice, {}};
            }
        }
        return {backendError(error), {}};
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
    enum class CommandKind { Copy, MoveStagingCopy, Remove };

    SshServerSideCopyBackend(ssh_session session, sftp_session sftp,
                             rfm::core::RemoteCopyMethod copyMethod,
                             rfm::core::NativeServerCopyPrimitive nativePrimitive)
        : m_session(session), m_sftp(sftp), m_copyMethod(copyMethod),
          m_nativePrimitive(nativePrimitive)
    {}

    ~SshServerSideCopyBackend() override
    {
        closeChannel();
        closeCopyHandles();
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

    rfm::core::RemoteBackendResult rename(const QString& source,
                                          const QString& destination) override
    {
        SftpBackend backend(m_sftp);
        return backend.rename(source, destination);
    }

    rfm::core::RemoteBackendResult startCopy(const QString& source, const QString& destination,
                                             bool recursive) override
    {
        if (source.isEmpty() || destination.isEmpty() || m_sftp == nullptr) {
            return {rfm::core::RemoteBackendError::InvalidPath, {}};
        }
        if (m_copyMethod == rfm::core::RemoteCopyMethod::NativeServerCopy) {
            if (m_nativePrimitive != rfm::core::NativeServerCopyPrimitive::PosixCp) {
                return {rfm::core::RemoteBackendError::Unsupported,
                        QStringLiteral("No executable native remote copy primitive is available.")};
            }
            const QString command =
                rfm::ssh::RemoteCopyCommand::build(source, destination, recursive);
            if (command.isEmpty()) {
                return {rfm::core::RemoteBackendError::InvalidPath, {}};
            }
            return startCommand(command, CommandKind::Copy);
        }
        if (m_copyMethod == rfm::core::RemoteCopyMethod::SftpCopyData) {
            return {rfm::core::RemoteBackendError::Unsupported,
                    QStringLiteral("SFTP copy-data is not executable by the current backend.")};
        }
        closeChannel();
        closeCopyHandles();
        m_copyTasks.clear();
        m_removeTasks.clear();
        m_removeRoot.clear();
        m_removeActive = false;
        m_copyBuffer.clear();
        m_copyBufferOffset = 0;
        m_copyRecursive = recursive;
        m_copyActive = true;
        m_copyCancellationPending = false;
        m_copyTasks.push_back({source, destination});
        return {};
    }

    rfm::core::RemoteBackendResult reserveStaging(const QString& path) override
    {
        const QByteArray encoded = path.toUtf8();
        if (sftp_mkdir(m_sftp, encoded.constData(), 0700) == SSH_OK) {
            return {};
        }
        const int error = sftp_get_error(m_sftp);
        if (error == SSH_FX_FILE_ALREADY_EXISTS) {
            return {rfm::core::RemoteBackendError::AlreadyExists, {}};
        }
        sftp_attributes existing = sftp_lstat(m_sftp, encoded.constData());
        if (existing != nullptr) {
            sftp_attributes_free(existing);
            return {rfm::core::RemoteBackendError::AlreadyExists, {}};
        }
        return {backendError(error), {}};
    }

    rfm::core::RemoteBackendResult removeEmptyDirectory(const QString& path) override
    {
        const QByteArray encoded = path.toUtf8();
        if (sftp_rmdir(m_sftp, encoded.constData()) == SSH_OK) {
            return {};
        }
        return {backendError(sftp_get_error(m_sftp)),
                QCoreApplication::translate(
                    "SshServerSideCopyBackend",
                    "SFTP rmdir could not remove the post-promotion staging directory. It may not "
                    "be empty or accessible.")};
    }

    rfm::core::RemoteBackendResult startMoveStagingCopy(const QString& source,
                                                        const QString& destination) override
    {
        const QString command = rfm::ssh::RemoteCopyCommand::buildMoveStaging(source, destination);
        if (command.isEmpty()) {
            return {rfm::core::RemoteBackendError::InvalidPath, {}};
        }
        return startCommand(command, CommandKind::MoveStagingCopy);
    }

    rfm::core::RemoteBackendResult startRemove(const QString& path, bool recursive,
                                               bool protectMountPoint) override
    {
        const QString normalizedPath = rfm::core::RemotePath::normalize(path);
        const QString basename = rfm::core::RemotePath::fileName(normalizedPath);
        if (recursive && basename.startsWith(QStringLiteral(".rfm-copy-")) &&
            basename.endsWith(QStringLiteral(".partial"))) {
            closeChannel();
            closeCopyHandles();
            m_removeTasks.clear();
            m_removeRoot = normalizedPath;
            m_removeActive = true;
            m_removeTasks.push_back({normalizedPath, false});
            return {};
        }
        const std::optional<QString> mountPointIdentity =
            protectMountPoint ? canonicalRemoteEntryPath(m_sftp, path)
                              : std::optional<QString>{QString{}};
        if (protectMountPoint && !mountPointIdentity.has_value()) {
            return {rfm::core::RemoteBackendError::Failure,
                    QCoreApplication::translate(
                        "SshServerSideCopyBackend",
                        "Unable to verify that the remote removal target is not a mount point.")};
        }
        const QString removalPath = protectMountPoint ? *mountPointIdentity : path;
        const QString command = rfm::ssh::RemoteCopyCommand::buildRemove(
            removalPath, recursive, protectMountPoint, mountPointIdentity.value_or(QString{}));
        if (command.isEmpty()) {
            return {rfm::core::RemoteBackendError::InvalidPath, {}};
        }
        return startCommand(command, CommandKind::Remove);
    }

    rfm::core::RemoteBackendResult startCommand(const QString& command, CommandKind kind)
    {
        closeChannel();
        m_commandKind = kind;
        m_channel = ssh_channel_new(m_session);
        if (m_channel == nullptr) {
            return {rfm::core::RemoteBackendError::Failure,
                    QCoreApplication::translate("SshServerSideCopyBackend",
                                                "Unable to open an SSH channel.")};
        }
        m_errorOutput.clear();
        m_standardOutput.clear();
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
        return m_copyActive ? pollSftpCopy() : pollCommand();
    }

    std::optional<rfm::core::RemoteBackendResult> pollRemove() override
    {
        return m_removeActive ? pollSftpRemove() : pollCommand();
    }

    std::optional<rfm::core::RemoteBackendResult> pollSftpCopy()
    {
        if (m_sftp == nullptr) {
            return finishSftpCopy({rfm::core::RemoteBackendError::Failure,
                                   QStringLiteral("The SFTP session is not available.")});
        }

        if (m_copySourceHandle != nullptr) {
            if (m_copyBufferOffset < m_copyBuffer.size()) {
                const qsizetype remaining = m_copyBuffer.size() - m_copyBufferOffset;
                const ssize_t written = sftp_write(m_copyDestinationHandle,
                                                   m_copyBuffer.constData() + m_copyBufferOffset,
                                                   static_cast<size_t>(remaining));
                if (written <= 0 || written > remaining) {
                    return finishSftpCopy(sftpOperationFailure(
                        QStringLiteral("Unable to write the remote copy destination.")));
                }
                m_copyBufferOffset += static_cast<qsizetype>(written);
                return std::nullopt;
            }

            char buffer[65'536];
            const ssize_t read = sftp_read(m_copySourceHandle, buffer, sizeof(buffer));
            if (read < 0) {
                return finishSftpCopy(
                    sftpOperationFailure(QStringLiteral("Unable to read the remote copy source.")));
            }
            if (read == 0) {
                const rfm::core::RemoteBackendResult closed = closeCopyHandlesResult();
                if (!closed.succeeded()) {
                    return finishSftpCopy(closed);
                }
                return std::nullopt;
            }
            m_copyBuffer = QByteArray(buffer, static_cast<qsizetype>(read));
            m_copyBufferOffset = 0;
            return std::nullopt;
        }

        if (m_copyTasks.isEmpty()) {
            m_copyActive = false;
            return rfm::core::RemoteBackendResult{};
        }

        const CopyTask task = m_copyTasks.takeLast();
        const QByteArray encodedSource = task.source.toUtf8();
        sftp_attributes attributes = sftp_lstat(m_sftp, encodedSource.constData());
        if (attributes == nullptr) {
            return finishSftpCopy(
                sftpOperationFailure(QStringLiteral("Unable to inspect the remote copy source.")));
        }

        if (attributes->type == SSH_FILEXFER_TYPE_SYMLINK) {
            sftp_attributes_free(attributes);
            // SFTP copy deliberately does not dereference or recreate links yet. Refusing the
            // entry is safer than silently turning it into a copy of its target.
            return finishSftpCopy({rfm::core::RemoteBackendError::Unsupported,
                                   QStringLiteral("Symbolic links are not supported by remote "
                                                  "SFTP copy.")});
        }

        if (attributes->type == SSH_FILEXFER_TYPE_DIRECTORY) {
            if (!m_copyRecursive) {
                sftp_attributes_free(attributes);
                return finishSftpCopy({rfm::core::RemoteBackendError::Failure,
                                       QStringLiteral("The remote copy source is a directory.")});
            }
            const unsigned int mode = attributes->permissions & 0777;
            const QByteArray encodedDestination = task.destination.toUtf8();
            if (sftp_mkdir(m_sftp, encodedDestination.constData(), mode == 0 ? 0700 : mode) !=
                SSH_OK) {
                sftp_attributes_free(attributes);
                return finishSftpCopy(sftpOperationFailure(
                    QStringLiteral("Unable to create a remote copy directory.")));
            }
            sftp_attributes_free(attributes);

            sftp_dir directory = sftp_opendir(m_sftp, encodedSource.constData());
            if (directory == nullptr) {
                return finishSftpCopy(
                    sftpOperationFailure(QStringLiteral("Unable to list the remote copy source.")));
            }
            QList<CopyTask> children;
            while (sftp_attributes child = sftp_readdir(m_sftp, directory)) {
                const QString name = QString::fromUtf8(child->name);
                if (name != QStringLiteral(".") && name != QStringLiteral("..")) {
                    const QString childSource = rfm::core::RemotePath::join(task.source, name);
                    const QString childDestination =
                        rfm::core::RemotePath::join(task.destination, name);
                    if (childSource.isEmpty() || childDestination.isEmpty() ||
                        !rfm::core::RemotePath::isValidName(name)) {
                        sftp_attributes_free(child);
                        sftp_closedir(directory);
                        return finishSftpCopy({rfm::core::RemoteBackendError::InvalidPath,
                                               QStringLiteral("The remote copy contains an invalid "
                                                              "entry name.")});
                    }
                    children.push_back({childSource, childDestination});
                }
                sftp_attributes_free(child);
            }
            const bool complete = sftp_dir_eof(directory) != 0;
            const int error = sftp_get_error(m_sftp);
            sftp_closedir(directory);
            if (!complete) {
                return finishSftpCopy(
                    {backendError(error), QStringLiteral("Unable to finish listing the remote copy "
                                                         "source.")});
            }
            for (auto iterator = children.crbegin(); iterator != children.crend(); ++iterator) {
                m_copyTasks.push_back(*iterator);
            }
            return std::nullopt;
        }

        if (attributes->type != SSH_FILEXFER_TYPE_REGULAR) {
            sftp_attributes_free(attributes);
            return finishSftpCopy({rfm::core::RemoteBackendError::Unsupported,
                                   QStringLiteral("The remote copy source is not a regular file "
                                                  "or directory.")});
        }

        const unsigned int mode = attributes->permissions & 0777;
        sftp_attributes_free(attributes);
        m_copySourceHandle = sftp_open(m_sftp, encodedSource.constData(), O_RDONLY, 0);
        if (m_copySourceHandle == nullptr) {
            return finishSftpCopy(
                sftpOperationFailure(QStringLiteral("Unable to open the remote copy source.")));
        }
        const QByteArray encodedDestination = task.destination.toUtf8();
        m_copyDestinationHandle = sftp_open(m_sftp, encodedDestination.constData(),
                                            O_WRONLY | O_CREAT | O_EXCL, mode == 0 ? 0600 : mode);
        if (m_copyDestinationHandle == nullptr) {
            const auto result = sftpOperationFailure(
                QStringLiteral("Unable to create the remote copy destination."));
            closeCopyHandles();
            return finishSftpCopy(result);
        }
        return std::nullopt;
    }

    std::optional<rfm::core::RemoteBackendResult> pollSftpRemove()
    {
        if (m_sftp == nullptr) {
            m_removeActive = false;
            return rfm::core::RemoteBackendResult{
                rfm::core::RemoteBackendError::Failure,
                QStringLiteral("The SFTP session is not available.")};
        }
        if (m_removeTasks.isEmpty()) {
            m_removeActive = false;
            m_removeRoot.clear();
            return rfm::core::RemoteBackendResult{};
        }

        const RemoveTask task = m_removeTasks.takeLast();
        const QString normalized = rfm::core::RemotePath::normalize(task.path);
        if (m_removeRoot.isEmpty() ||
            (normalized != m_removeRoot && !normalized.startsWith(m_removeRoot + QChar{'/'}))) {
            m_removeActive = false;
            m_removeTasks.clear();
            return rfm::core::RemoteBackendResult{
                rfm::core::RemoteBackendError::InvalidPath,
                QStringLiteral("Remote staging cleanup escaped its owned path.")};
        }
        const QByteArray encoded = normalized.toUtf8();
        sftp_attributes attributes = sftp_lstat(m_sftp, encoded.constData());
        if (attributes == nullptr) {
            if (backendError(sftp_get_error(m_sftp)) == rfm::core::RemoteBackendError::NotFound) {
                return std::nullopt;
            }
            m_removeActive = false;
            return sftpOperationFailure(QStringLiteral("Unable to inspect remote staging data."));
        }

        if (task.removeDirectory) {
            const bool directory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
            sftp_attributes_free(attributes);
            if (!directory) {
                m_removeActive = false;
                return rfm::core::RemoteBackendResult{
                    rfm::core::RemoteBackendError::Failure,
                    QStringLiteral("Remote staging changed while it was being cleaned.")};
            }
            if (sftp_rmdir(m_sftp, encoded.constData()) != SSH_OK) {
                m_removeActive = false;
                return sftpOperationFailure(
                    QStringLiteral("Unable to remove the remote staging directory."));
            }
            return std::nullopt;
        }

        if (attributes->type == SSH_FILEXFER_TYPE_DIRECTORY) {
            sftp_attributes_free(attributes);
            sftp_dir directory = sftp_opendir(m_sftp, encoded.constData());
            if (directory == nullptr) {
                m_removeActive = false;
                return sftpOperationFailure(QStringLiteral("Unable to list remote staging data."));
            }
            QList<QString> children;
            while (sftp_attributes child = sftp_readdir(m_sftp, directory)) {
                const QString name = QString::fromUtf8(child->name);
                if (name != QStringLiteral(".") && name != QStringLiteral("..")) {
                    if (!rfm::core::RemotePath::isValidName(name)) {
                        sftp_attributes_free(child);
                        sftp_closedir(directory);
                        m_removeActive = false;
                        return rfm::core::RemoteBackendResult{
                            rfm::core::RemoteBackendError::InvalidPath,
                            QStringLiteral("Remote staging contains an invalid entry name.")};
                    }
                    children.push_back(rfm::core::RemotePath::join(normalized, name));
                }
                sftp_attributes_free(child);
            }
            const bool complete = sftp_dir_eof(directory) != 0;
            const int error = sftp_get_error(m_sftp);
            sftp_closedir(directory);
            if (!complete) {
                m_removeActive = false;
                return rfm::core::RemoteBackendResult{
                    backendError(error),
                    QStringLiteral("Unable to finish listing remote staging data.")};
            }
            m_removeTasks.push_back({normalized, true});
            for (auto iterator = children.crbegin(); iterator != children.crend(); ++iterator) {
                m_removeTasks.push_back({*iterator, false});
            }
            return std::nullopt;
        }

        sftp_attributes_free(attributes);
        if (sftp_unlink(m_sftp, encoded.constData()) != SSH_OK) {
            m_removeActive = false;
            return sftpOperationFailure(QStringLiteral("Unable to remove remote staging data."));
        }
        return std::nullopt;
    }

    rfm::core::RemoteBackendResult sftpOperationFailure(const QString& detail) const
    {
        const rfm::core::RemoteBackendError error = backendError(sftp_get_error(m_sftp));
        return {error == rfm::core::RemoteBackendError::None
                    ? rfm::core::RemoteBackendError::Failure
                    : error,
                detail};
    }

    rfm::core::RemoteBackendResult closeCopyHandlesResult()
    {
        rfm::core::RemoteBackendResult result;
        if (m_copySourceHandle != nullptr) {
            if (sftp_close(m_copySourceHandle) != SSH_OK) {
                result = sftpOperationFailure(QStringLiteral("Unable to close the remote copy "
                                                             "source."));
            }
            m_copySourceHandle = nullptr;
        }
        if (m_copyDestinationHandle != nullptr) {
            if (sftp_close(m_copyDestinationHandle) != SSH_OK && result.succeeded()) {
                result = sftpOperationFailure(QStringLiteral("Unable to close the remote copy "
                                                             "destination."));
            }
            m_copyDestinationHandle = nullptr;
        }
        m_copyBuffer.clear();
        m_copyBufferOffset = 0;
        return result;
    }

    void closeCopyHandles() { static_cast<void>(closeCopyHandlesResult()); }

    std::optional<rfm::core::RemoteBackendResult>
    finishSftpCopy(const rfm::core::RemoteBackendResult& result)
    {
        closeCopyHandles();
        m_copyTasks.clear();
        m_copyActive = false;
        return result;
    }

    std::optional<rfm::core::RemoteBackendResult> pollCommand()
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
        if (outputBytes > 0 && m_standardOutput.size() < 2048) {
            const int remaining = 2048 - static_cast<int>(m_standardOutput.size());
            m_standardOutput.append(buffer, std::min(outputBytes, remaining));
        } else if (outputBytes == SSH_ERROR) {
            closeChannel();
            return rfm::core::RemoteBackendResult{
                rfm::core::RemoteBackendError::Failure,
                QCoreApplication::translate("SshServerSideCopyBackend",
                                            "Unable to drain the remote copy channel.")};
        }
        if (ssh_channel_is_eof(m_channel) == 0) {
            return std::nullopt;
        }

        const bool copyCommand =
            m_commandKind == CommandKind::Copy || m_commandKind == CommandKind::MoveStagingCopy;
        const std::optional<quint32> copyStatus =
            copyCommand ? rfm::ssh::RemoteCopyCommand::parseCopyStatus(m_standardOutput)
                        : std::optional<quint32>{};
        const bool completionReceived = copyCommand ? copyStatus.has_value() : m_exitStateReceived;
        if (!completionReceived && ++m_exitStatePolls < 100) {
            return std::nullopt;
        }
        const QString detail = QString::fromUtf8(m_errorOutput).trimmed();
        const bool exitStateReceived = m_exitStateReceived;
        const CommandKind commandKind = m_commandKind;
        const uint32_t exitCode = copyStatus.value_or(m_exitCode);
        const bool inconsistentStatus =
            copyStatus.has_value() && exitStateReceived && *copyStatus != m_exitCode;
        closeChannel();
        if (inconsistentStatus) {
            return rfm::core::RemoteBackendResult{
                rfm::core::RemoteBackendError::Failure,
                QCoreApplication::translate(
                    "SshServerSideCopyBackend",
                    "The remote copy reported inconsistent completion status.")};
        }
        if ((commandKind == CommandKind::Copy || commandKind == CommandKind::MoveStagingCopy) &&
            !copyStatus.has_value()) {
            return rfm::core::RemoteBackendResult{
                rfm::core::RemoteBackendError::Failure,
                QCoreApplication::translate("SshServerSideCopyBackend",
                                            "Unable to verify completion of the remote copy.")};
        }
        if ((copyCommand || exitStateReceived) && exitCode == 0) {
            return rfm::core::RemoteBackendResult{};
        }
        if (exitCode == 126 || exitCode == 127) {
            return rfm::core::RemoteBackendResult{
                rfm::core::RemoteBackendError::Unsupported,
                commandKind == CommandKind::Copy || commandKind == CommandKind::MoveStagingCopy
                    ? QCoreApplication::translate(
                          "SshServerSideCopyBackend",
                          "The 'cp' command is not available on the server.")
                    : QCoreApplication::translate(
                          "SshServerSideCopyBackend",
                          "The 'rm' command is not available on the server.")};
        }
        return rfm::core::RemoteBackendResult{
            rfm::core::RemoteBackendError::Failure,
            detail.isEmpty()
                ? (commandKind == CommandKind::Copy || commandKind == CommandKind::MoveStagingCopy
                       ? QCoreApplication::translate("SshServerSideCopyBackend",
                                                     "Remote copy failed.")
                       : QCoreApplication::translate("SshServerSideCopyBackend",
                                                     "Remote source removal failed."))
                : detail};
    }

    std::optional<rfm::core::RemoteBackendResult> requestCopyCancellation() override
    {
        if (m_copyActive) {
            closeCopyHandles();
            m_copyTasks.clear();
            m_copyActive = false;
            m_copyCancellationPending = true;
            return rfm::core::RemoteBackendResult{};
        }
        if (m_copyCancellationPending) {
            return rfm::core::RemoteBackendResult{};
        }
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
        if (m_copyCancellationPending) {
            m_copyCancellationPending = false;
            return rfm::core::RemoteBackendResult{};
        }
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
    struct CopyTask {
        QString source;
        QString destination;
    };

    struct RemoveTask {
        QString path;
        bool removeDirectory{false};
    };

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
    rfm::core::RemoteCopyMethod m_copyMethod{rfm::core::RemoteCopyMethod::ClientMediatedSftp};
    rfm::core::NativeServerCopyPrimitive m_nativePrimitive{
        rfm::core::NativeServerCopyPrimitive::None};
    ssh_channel m_channel{nullptr};
    QList<CopyTask> m_copyTasks;
    QList<RemoveTask> m_removeTasks;
    QString m_removeRoot;
    QByteArray m_copyBuffer;
    qsizetype m_copyBufferOffset{0};
    sftp_file m_copySourceHandle{nullptr};
    sftp_file m_copyDestinationHandle{nullptr};
    bool m_copyRecursive{false};
    bool m_copyActive{false};
    bool m_copyCancellationPending{false};
    bool m_removeActive{false};
    QByteArray m_errorOutput;
    QByteArray m_standardOutput;
    int m_exitStatePolls{0};
    bool m_exitStateReceived{false};
    uint32_t m_exitCode{UINT32_MAX};
    ssh_channel_callbacks_struct m_channelCallbacks{};
    CancellationPhase m_cancellationPhase{CancellationPhase::NotRequested};
    CommandKind m_commandKind{CommandKind::Copy};
    QElapsedTimer m_cancellationTimer;
};

struct SshCommandPollResult {
    std::optional<rfm::core::VolumeCommandResult> result;
    bool connectionLost{false};
    bool activityAvailable{false};
};

class SshCommandProcess final
{
  public:
    explicit SshCommandProcess(
        ssh_session session,
        qint64 timeout = rfm::ssh::SshCommandPollScheduler::commandTimeoutMilliseconds)
        : m_session(session), m_timeoutMilliseconds(timeout)
    {}
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
            return {{}, true, false};
        }
        char buffer[4096];
        const int errorBytes = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 1);
        if (errorBytes == SSH_ERROR) {
            const bool lost = ssh_is_connected(m_session) == 0;
            const QString output = QString::fromUtf8(m_standardOutput);
            const QString error = QString::fromUtf8(m_standardError);
            closeChannel();
            return lost ? SshCommandPollResult{{}, true, false}
                        : SshCommandPollResult{rfm::core::VolumeCommandResult{true, false, true, -1,
                                                                              output, error, false},
                                               false, false};
        }
        appendBounded(m_standardError, buffer, errorBytes);
        const int outputBytes = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 0);
        if (outputBytes == SSH_ERROR) {
            const bool lost = ssh_is_connected(m_session) == 0;
            const QString output = QString::fromUtf8(m_standardOutput);
            const QString error = QString::fromUtf8(m_standardError);
            closeChannel();
            return lost ? SshCommandPollResult{{}, true, false}
                        : SshCommandPollResult{rfm::core::VolumeCommandResult{true, false, true, -1,
                                                                              output, error, false},
                                               false, false};
        }
        appendBounded(m_standardOutput, buffer, outputBytes);
        if (m_timer.elapsed() >= m_timeoutMilliseconds) {
            const QString output = QString::fromUtf8(m_standardOutput);
            const QString error = QString::fromUtf8(m_standardError);
            closeChannel();
            return {rfm::core::VolumeCommandResult{true, true, false, -1, output, error, false},
                    false, false};
        }
        if (errorBytes > 0 || outputBytes > 0) {
            return {{}, false, true};
        }
        if (ssh_channel_is_eof(m_channel) == 0) {
            return {{}, false, false};
        }
        if (!m_exitStatusReceived && ++m_exitStatusPolls < 100) {
            return {{}, false, false};
        }
        const int exitCode = m_exitStatusReceived ? m_exitCode : -1;
        const QString output = QString::fromUtf8(m_standardOutput);
        const QString error = QString::fromUtf8(m_standardError);
        closeChannel();
        return {rfm::core::VolumeCommandResult{true, false, false, exitCode, output, error, false},
                false, false};
    }

  private:
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
    qint64 m_timeoutMilliseconds{rfm::ssh::SshCommandPollScheduler::commandTimeoutMilliseconds};
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
    bool activityAvailable{false};
    std::optional<rfm::core::VolumeOperationError> protocolError;
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

    bool start(const QString& command, rfm::core::SecurePassword password)
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
            return {{}, true, false, {}};
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
                    return {{}, true, false, {}};
                }
                return {technicalFailure(QStringLiteral("The interactive SSH channel failed.")),
                        false,
                        false,
                        {}};
            }
            if (bytes > 0) {
                m_activityAvailable = true;
                const auto event = m_parser.consume(QByteArray(buffer, bytes));
                if (event == rfm::ssh::RemotePolkitPromptEvent::PasswordPrompt) {
                    m_promptSeen = true;
                    if (!m_password.appendLineFeed()) {
                        clearSecret();
                        closeChannel();
                        return {technicalFailure(
                                    QStringLiteral("Unable to prepare Polkit credentials.")),
                                false,
                                false,
                                {}};
                    }
                } else if (event == rfm::ssh::RemotePolkitPromptEvent::AuthenticationFailed) {
                    clearSecret();
                    closeChannel();
                    return {expectedFailure(), false, false,
                            rfm::core::VolumeOperationError::AuthenticationFailed};
                }
            }
        }

        if (m_promptSeen && !m_password.isEmpty()) {
            const std::size_t remaining = m_password.remainingSize();
            const int written = ssh_channel_write(m_channel, m_password.remainingData(),
                                                  static_cast<uint32_t>(std::min<std::size_t>(
                                                      remaining, std::numeric_limits<int>::max())));
            if (written == SSH_ERROR) {
                const bool lost = ssh_is_connected(m_session) == 0;
                clearSecret();
                closeChannel();
                return lost ? SshInteractiveCommandPollResult{{}, true, false, {}}
                            : SshInteractiveCommandPollResult{
                                  technicalFailure(
                                      QStringLiteral("Unable to send Polkit credentials.")),
                                  false,
                                  false,
                                  {}};
            }
            if (written > 0) {
                if (m_password.consumeWritten(static_cast<std::size_t>(written))) {
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
            return {rfm::core::VolumeCommandResult{true, true, false, -1, {}, detail, false},
                    false,
                    false,
                    {}};
        }
        if (ssh_channel_is_eof(m_channel) == 0) {
            return {{}, false, std::exchange(m_activityAvailable, false), {}};
        }
        if (!m_exitStatusReceived && ++m_exitStatusPolls < 100) {
            return {{}, false, std::exchange(m_activityAvailable, false), {}};
        }

        const int exitCode = m_exitStatusReceived ? m_exitCode : -1;
        std::optional<rfm::core::VolumeOperationError> protocolError;
        if (exitCode != 0) {
            protocolError = m_parser.operationError();
            if (!protocolError.has_value() && m_passwordWasSent &&
                !m_parser.authenticationCompleted()) {
                protocolError = rfm::core::VolumeOperationError::AuthenticationFailed;
            }
        }
        clearSecret();
        closeChannel();
        return {rfm::core::VolumeCommandResult{true, false, false, exitCode, {}, {}, false}, false,
                false, protocolError};
    }

  private:
    static constexpr qint64 timeoutMilliseconds =
        rfm::ssh::SshCommandPollScheduler::commandTimeoutMilliseconds;

    static rfm::core::VolumeCommandResult technicalFailure(QString diagnostic)
    {
        return {true, false, true, -1, {}, std::move(diagnostic), false};
    }

    static rfm::core::VolumeCommandResult expectedFailure()
    {
        return {true, false, false, 1, {}, {}, false};
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

    void clearSecret() { m_password.clear(); }

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
    rfm::core::SecurePassword m_password;
    QElapsedTimer m_timer;
    int m_exitStatusPolls{0};
    int m_exitCode{-1};
    bool m_exitStatusReceived{false};
    bool m_promptSeen{false};
    bool m_passwordWasSent{false};
    bool m_activityAvailable{false};
};

enum class SshVolumeCommandPurpose {
    Capabilities,
    BlockDevices,
    RevalidateVolumeOperation,
    VolumeOperation,
    InteractiveVolumeOperation
};

struct SshVolumeCommandTask {
    SshVolumeCommandPurpose purpose{SshVolumeCommandPurpose::Capabilities};
    QString command;
    quint64 storageRequestId{0};
    rfm::core::VolumeOperationRequest operationRequest;
    rfm::core::SecurePassword password;
};

struct AwaitingVolumeAuthentication {
    rfm::core::VolumeOperationRequest request;
    quint64 authenticationToken{0};
};

bool isTerminalTransferState(rfm::core::TransferState state)
{
    return state == rfm::core::TransferState::Completed ||
           state == rfm::core::TransferState::Cancelled ||
           state == rfm::core::TransferState::Failed;
}

QString remoteCopyMethodDescription(rfm::core::RemoteCopyMethod method,
                                    rfm::core::NativeServerCopyPrimitive primitive)
{
    switch (method) {
    case rfm::core::RemoteCopyMethod::SftpCopyData:
        return QStringLiteral("SftpCopyData");
    case rfm::core::RemoteCopyMethod::NativeServerCopy:
        return primitive == rfm::core::NativeServerCopyPrimitive::PosixCp
                   ? QStringLiteral("NativeServerCopy (PosixCp)")
                   : QStringLiteral("NativeServerCopy (unavailable primitive)");
    case rfm::core::RemoteCopyMethod::ClientMediatedSftp:
        return QStringLiteral("ClientMediatedSftp");
    }
    return QStringLiteral("Unknown");
}

} // namespace

namespace rfm::ssh
{

class SshSession::Impl final
{
  public:
    Impl() = default;

    Impl(TransferBackendFactory backendFactory, std::function<bool()> connectionAvailable)
        : transferBackendFactory(std::move(backendFactory)),
          transferConnectionAvailable(std::move(connectionAvailable))
    {}

    ~Impl() { reset(); }

    [[nodiscard]] bool transfersAvailable() const
    {
        return transferConnectionAvailable ? transferConnectionAvailable() : sftp != nullptr;
    }

    [[nodiscard]] std::unique_ptr<rfm::core::RemoteTransferBackend> makeTransferBackend()
    {
        if (transferBackendFactory) {
            return transferBackendFactory();
        }
        return std::make_unique<SftpTransferBackend>(session, sftp);
    }

    void reset()
    {
        remoteCopyCapabilityProcess.reset();
        remoteCopyCapabilityPollScheduler.cancel();
        storageCapabilityProcess.reset();
        storageCapabilityPollScheduler.cancel();
        storageCapabilityLifecycle.reset();
        volumeCommandProcess.reset();
        interactiveVolumeCommandProcess.reset();
        activeVolumeCommand.reset();
        volumeCommandQueue.clear();
        activeVolumeDevices.clear();
        pendingStorageRequestId = 0;
        pendingBlockDevices.clear();
        pendingVolumeOperations.clear();
        awaitingVolumeAuthentications.clear();
        volumePollScheduler.cancel();
        volumeCapabilityCache.reset();
        currentServerCapabilities = {};
        remoteCopyExecutionCapabilities = {};
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
        terminalTransferIds.clear();
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
        awaitingHostConfirmation = false;
        awaitingPasswordAuthentication = false;
    }

    ssh_session session{nullptr};
    sftp_session sftp{nullptr};
    rfm::core::ConnectionProfile profile;
    bool awaitingHostConfirmation{false};
    bool awaitingPasswordAuthentication{false};
    // Declaration order is intentional: the job dies before its backend, and
    // both are reset before the SFTP session in reset().
    std::unique_ptr<rfm::core::RemoteTransferBackend> transferBackend;
    std::unique_ptr<rfm::core::TransferJob> activeTransferJob;
    std::unique_ptr<SshServerSideCopyBackend> copyBackend;
    std::unique_ptr<rfm::core::ServerSideCopyJob> activeCopyJob;
    std::unique_ptr<rfm::ssh::RemoteStorageScanner> storageScanner;
    std::unique_ptr<SshCommandProcess> remoteCopyCapabilityProcess;
    std::unique_ptr<SshCommandProcess> storageCapabilityProcess;
    std::unique_ptr<SshCommandProcess> volumeCommandProcess;
    std::unique_ptr<SshInteractivePolkitProcess> interactiveVolumeCommandProcess;
    std::optional<SshVolumeCommandTask> activeVolumeCommand;
    std::deque<SshVolumeCommandTask> volumeCommandQueue;
    QSet<QString> activeVolumeDevices;
    QQueue<rfm::core::VolumeOperationRequest> pendingVolumeOperations;
    QHash<quint64, AwaitingVolumeAuthentication> awaitingVolumeAuthentications;
    QList<rfm::core::LinuxBlockDevice> pendingBlockDevices;
    rfm::ssh::RemoteLinuxVolumeCapabilityCache volumeCapabilityCache;
    rfm::core::ServerCapabilities currentServerCapabilities;
    rfm::core::RemoteCopyExecutionCapabilities remoteCopyExecutionCapabilities;
    rfm::ssh::SshCommandPollScheduler remoteCopyCapabilityPollScheduler;
    rfm::ssh::SshCommandPollScheduler storageCapabilityPollScheduler;
    rfm::ssh::RemoteStorageCapabilityLifecycle storageCapabilityLifecycle;
    rfm::ssh::SshCommandPollScheduler volumePollScheduler;
    sftp_file storageProbeFile{nullptr};
    QByteArray storageProbeData;
    quint64 storageProbeRequestId{0};
    quint64 pendingStorageRequestId{0};
    QSet<quint64> terminalTransferIds;
    TransferBackendFactory transferBackendFactory;
    std::function<bool()> transferConnectionAvailable;
    bool transferStepScheduled{false};
    bool copyStepScheduled{false};
    bool storageStepScheduled{false};
    bool storageProbeStepScheduled{false};
    bool shuttingDown{false};
    bool disconnecting{false};
    quint64 nextAuthenticationToken{0};
};

SshSession::SshSession(QObject* parent) : QObject(parent), m_impl(std::make_unique<Impl>())
{
    qRegisterMetaType<rfm::core::ServerCapabilities>();
    qRegisterMetaType<rfm::core::RemoteCopyExecutionCapabilities>();
    qRegisterMetaType<rfm::core::RemoteStorageCapabilities>();
}

SshSession::SshSession(TransferBackendFactory transferBackendFactory,
                       std::function<bool()> transferConnectionAvailable, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(std::move(transferBackendFactory),
                                                     std::move(transferConnectionAvailable)))
{
    qRegisterMetaType<rfm::core::ServerCapabilities>();
    qRegisterMetaType<rfm::core::RemoteCopyExecutionCapabilities>();
    qRegisterMetaType<rfm::core::RemoteStorageCapabilities>();
}

SshSession::~SshSession()
{
    terminalizeTransfer(rfm::core::TransferState::Cancelled,
                        tr("Transfer cancelled because the SSH session is closing."));
}

void SshSession::postVolumeAuthentication(quint64 operationId, quint64 authenticationToken,
                                          rfm::core::SecurePassword password)
{
    QCoreApplication::postEvent(
        this, new VolumeAuthenticationEvent(operationId, authenticationToken, std::move(password)));
}

void SshSession::postPasswordAuthentication(rfm::core::SecurePassword password)
{
    QCoreApplication::postEvent(this, new PasswordAuthenticationEvent(std::move(password)));
}

bool SshSession::event(QEvent* event)
{
    if (event->type() == passwordAuthenticationEventType()) {
        auto* const authentication = static_cast<PasswordAuthenticationEvent*>(event);
        authenticateWithPassword(std::move(authentication->password));
        return true;
    }
    if (event->type() == volumeAuthenticationEventType()) {
        auto* const authentication = static_cast<VolumeAuthenticationEvent*>(event);
        authenticateVolume(authentication->operationId, authentication->authenticationToken,
                           std::move(authentication->password));
        return true;
    }
    return QObject::event(event);
}

void SshSession::connectToHost(rfm::core::ConnectionProfile profile)
{
    terminalizeTransfer(rfm::core::TransferState::Cancelled,
                        tr("Transfer cancelled because the SSH session was replaced."));
    m_impl->reset();
    if (!profile.isValid()) {
        fail(tr("Invalid connection settings."));
        return;
    }

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

    const QString resolvedPrivateKeyPath =
        SshAuthenticationPolicy::resolvePrivateKeyPath(m_impl->profile.privateKeyPath);
    if (!resolvedPrivateKeyPath.isEmpty()) {
        const QByteArray identity = resolvedPrivateKeyPath.toUtf8();
        if (ssh_options_set(m_impl->session, SSH_OPTIONS_IDENTITY, identity.constData()) !=
            SSH_OK) {
            fail(tr("Unable to configure the SSH private key: %1")
                     .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
            return;
        }
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
    if (m_impl->profile.authenticationMode == rfm::core::AuthenticationMode::PasswordOnly) {
        const int auth = ssh_userauth_none(m_impl->session, nullptr);
        switch (auth) {
        case SSH_AUTH_SUCCESS:
            // The server accepted the "none" authentication.
            // Continue with normal success path.
            openSftp();
            return;
        case SSH_AUTH_DENIED:
        case SSH_AUTH_PARTIAL:
            // Server requires authentication, check what methods are available
            {
                const int methods = ssh_userauth_list(m_impl->session, nullptr);
                if (methods < 0) {
                    fail(tr("Unable to determine the server authentication methods: %1")
                             .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
                    return;
                }
                if ((static_cast<unsigned int>(methods) & SSH_AUTH_METHOD_PASSWORD) == 0U) {
                    fail(tr("The server does not offer password authentication."));
                    return;
                }
                m_impl->awaitingPasswordAuthentication = true;
                emit passwordAuthenticationRequired(PasswordAuthenticationReason::PasswordOnly);
                return;
            }
        case SSH_AUTH_ERROR:
            fail(tr("SSH authentication could not be completed: %1")
                     .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
            return;
        case SSH_AUTH_AGAIN:
            // This should normally not happen in this context
            // But if it does, treat as error
            fail(tr("SSH authentication could not be completed: %1")
                     .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
            return;
        default:
            // Unexpected result
            fail(tr("SSH authentication could not be completed: %1")
                     .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
            return;
        }
    }

    const QString resolvedPrivateKeyPath =
        SshAuthenticationPolicy::resolvePrivateKeyPath(m_impl->profile.privateKeyPath);
    if (!resolvedPrivateKeyPath.isEmpty()) {
        const QByteArray identity = resolvedPrivateKeyPath.toUtf8();
        ssh_key privateKey = nullptr;
        const int imported =
            ssh_pki_import_privkey_file(identity.constData(), "", nullptr, nullptr, &privateKey);
        if (privateKey != nullptr) {
            ssh_key_free(privateKey);
        }
        if (imported != SSH_OK) {
            fail(tr("Unable to load the configured private key “%1”. Check the path and file "
                    "permissions. RFM cannot use a passphrase-protected explicit key: load it in "
                    "an SSH agent and leave the Private key field empty. RFM does not request or "
                    "store key passphrases.")
                     .arg(m_impl->profile.privateKeyPath));
            return;
        }
    }

    // An empty passphrase prevents libssh from invoking a terminal prompt. Unlocked agent keys
    // and unencrypted identity files remain available to the automatic authentication pass.
    const int auth = ssh_userauth_publickey_auto(m_impl->session, nullptr, "");
    const int methods = auth == SSH_AUTH_SUCCESS ? 0 : ssh_userauth_list(m_impl->session, nullptr);
    const AuthenticationNextStep next = SshAuthenticationPolicy::afterPasswordless(
        authenticationResult(auth), m_impl->profile.allowPasswordAuthentication,
        methods >= 0 && (static_cast<unsigned int>(methods) & SSH_AUTH_METHOD_PASSWORD) != 0U);
    if (next == AuthenticationNextStep::RequestPassword) {
        m_impl->awaitingPasswordAuthentication = true;
        emit passwordAuthenticationRequired(SshAuthenticationPolicy::passwordPromptReason(
            m_impl->profile.authenticationMode, authenticationResult(auth),
            !m_impl->profile.privateKeyPath.trimmed().isEmpty()));
        return;
    }
    if (next == AuthenticationNextStep::Reject) {
        fail(tr("Authentication failed. Check your SSH agent, keys, or password."));
        return;
    }
    if (next == AuthenticationNextStep::Fail) {
        fail(tr("SSH authentication could not be completed: %1")
                 .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
        return;
    }

    openSftp();
}

void SshSession::authenticateWithPassword(rfm::core::SecurePassword password)
{
    if (!m_impl->awaitingPasswordAuthentication || m_impl->session == nullptr ||
        password.isEmpty()) {
        password.clear();
        return;
    }

    const int auth = ssh_userauth_password(m_impl->session, nullptr, password.remainingData());
    password.clear();
    if (auth == SSH_AUTH_SUCCESS) {
        m_impl->awaitingPasswordAuthentication = false;
        openSftp();
        return;
    }
    if (auth == SSH_AUTH_DENIED) {
        emit passwordAuthenticationRejected(tr("Incorrect password. Please try again."));
        return;
    }
    fail(tr("SSH password authentication could not be completed: %1")
             .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
}

void SshSession::cancelPasswordAuthentication()
{
    if (!m_impl->awaitingPasswordAuthentication) {
        return;
    }
    m_impl->reset();
}

void SshSession::openSftp()
{
    m_impl->sftp = sftp_new(m_impl->session);
    if (m_impl->sftp == nullptr || sftp_init(m_impl->sftp) != SSH_OK) {
        fail(tr("Unable to start the SFTP subsystem: %1")
                 .arg(QString::fromUtf8(ssh_get_error(m_impl->session))));
        return;
    }

    const int sftpProtocolVersion = sftp_server_version(m_impl->sftp);
    m_impl->currentServerCapabilities = rfm::core::detectedServerCapabilities(
        announcedSftpExtensions(m_impl->sftp), QDateTime::currentDateTimeUtc(),
        sftpProtocolVersion < 0 ? std::nullopt : std::optional<int>{sftpProtocolVersion});
    emit serverCapabilitiesDetected(m_impl->profile, m_impl->currentServerCapabilities);

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
            entries.push_back({name, attributes->size, remoteModificationTime(attributes),
                               attributes->type == SSH_FILEXFER_TYPE_DIRECTORY,
                               attributes->type == SSH_FILEXFER_TYPE_SYMLINK,
                               name.startsWith(QChar{'.'})});
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
    startRemoteCopyCapabilityProbe();
    startRemoteStorageCapabilityProbe();
    emit connected(initialPath, entries);
}

void SshSession::startRemoteCopyCapabilityProbe()
{
    m_impl->remoteCopyExecutionCapabilities = rfm::ssh::RemoteCopyCapabilityProbe::unavailable();
    m_impl->remoteCopyCapabilityProcess = std::make_unique<SshCommandProcess>(m_impl->session);
    if (!m_impl->remoteCopyCapabilityProcess->start(
            rfm::ssh::RemoteCopyCapabilityProbe::command())) {
        m_impl->remoteCopyCapabilityProcess.reset();
        emit remoteCopyExecutionCapabilitiesDetected(m_impl->remoteCopyExecutionCapabilities);
        return;
    }
    scheduleRemoteCopyCapabilityProbe();
}

void SshSession::scheduleRemoteCopyCapabilityProbe(bool activityAvailable)
{
    if (m_impl->remoteCopyCapabilityProcess == nullptr) {
        return;
    }
    const auto schedule = m_impl->remoteCopyCapabilityPollScheduler.schedule(activityAvailable);
    if (schedule.has_value()) {
        QTimer::singleShot(schedule->delayMilliseconds, this, [this, schedule] {
            if (m_impl->remoteCopyCapabilityPollScheduler.consume(schedule->generation)) {
                processRemoteCopyCapabilityProbe();
            }
        });
    }
}

void SshSession::processRemoteCopyCapabilityProbe()
{
    if (m_impl->remoteCopyCapabilityProcess == nullptr) {
        return;
    }
    const SshCommandPollResult poll = m_impl->remoteCopyCapabilityProcess->poll();
    if (!poll.connectionLost && !poll.result.has_value()) {
        scheduleRemoteCopyCapabilityProbe(poll.activityAvailable);
        return;
    }
    if (poll.result.has_value()) {
        m_impl->remoteCopyExecutionCapabilities =
            rfm::ssh::RemoteCopyCapabilityProbe::capabilitiesFrom(*poll.result);
    } else {
        m_impl->remoteCopyExecutionCapabilities =
            rfm::ssh::RemoteCopyCapabilityProbe::unavailable();
    }
    m_impl->remoteCopyCapabilityProcess.reset();
    emit remoteCopyExecutionCapabilitiesDetected(m_impl->remoteCopyExecutionCapabilities);
}

void SshSession::startRemoteStorageCapabilityProbe()
{
    m_impl->currentServerCapabilities.storage = {};
    m_impl->storageCapabilityLifecycle.reset();
    startNextRemoteStorageCapabilityProbe();
}

void SshSession::startNextRemoteStorageCapabilityProbe()
{
    constexpr qint64 capabilityProbeTimeoutMilliseconds = 10'000;
    while (m_impl->session != nullptr && ssh_is_connected(m_impl->session) != 0) {
        const auto stage = m_impl->storageCapabilityLifecycle.stage();
        if (stage == rfm::ssh::RemoteStorageCapabilityProbeStage::Posix ||
            stage == rfm::ssh::RemoteStorageCapabilityProbeStage::WindowsPowerShell) {
            const QString command =
                stage == rfm::ssh::RemoteStorageCapabilityProbeStage::Posix
                    ? rfm::ssh::RemoteStorageCapabilityProbe::posixCommand()
                    : rfm::ssh::RemoteStorageCapabilityProbe::windowsPowerShellCommand();
            m_impl->storageCapabilityProcess = std::make_unique<SshCommandProcess>(
                m_impl->session, capabilityProbeTimeoutMilliseconds);
            if (m_impl->storageCapabilityProcess->start(command)) {
                scheduleRemoteStorageCapabilityProbe();
                return;
            }
            m_impl->storageCapabilityProcess.reset();
            if (stage == rfm::ssh::RemoteStorageCapabilityProbeStage::Posix) {
                m_impl->storageCapabilityLifecycle.skipPosix();
                m_impl->volumeCapabilityCache.update({true, false, false, false, false});
            } else {
                m_impl->storageCapabilityLifecycle.skipWindows();
            }
            continue;
        }
        if (stage == rfm::ssh::RemoteStorageCapabilityProbeStage::MountInfo) {
            const QByteArray path = QByteArrayLiteral("/proc/self/mountinfo");
            sftp_file mountInfo = sftp_open(m_impl->sftp, path.constData(), O_RDONLY, 0);
            if (mountInfo == nullptr) {
                const int status = sftp_get_error(m_impl->sftp);
                if (isFatalSftpError(status) || ssh_is_connected(m_impl->session) == 0) {
                    fail(tr("The SSH connection was lost while probing remote storage "
                            "capabilities."));
                    return;
                }
                m_impl->storageCapabilityLifecycle.recordMountInfo(
                    rfm::ssh::RemoteStorageCapabilityProbe::mountInfoCapabilityFromSftpStatus(
                        status, false));
                finishRemoteStorageCapabilityProbe();
                return;
            }
            char byte = '\0';
            const ssize_t bytesRead = sftp_read(mountInfo, &byte, 1);
            const int status = bytesRead >= 0 ? SSH_FX_OK : sftp_get_error(m_impl->sftp);
            sftp_close(mountInfo);
            if (bytesRead < 0) {
                if (isFatalSftpError(status) || ssh_is_connected(m_impl->session) == 0) {
                    fail(tr("The SSH connection was lost while reading remote mount "
                            "information."));
                } else {
                    fail(tr("Unable to read remote mount information after opening it."));
                }
                return;
            }
            m_impl->storageCapabilityLifecycle.recordMountInfo(
                rfm::core::CapabilitySupport::Supported);
            finishRemoteStorageCapabilityProbe();
            return;
        }
        finishRemoteStorageCapabilityProbe();
        return;
    }
    fail(tr("The SSH connection was lost while probing remote storage capabilities."));
}

void SshSession::scheduleRemoteStorageCapabilityProbe(bool activityAvailable)
{
    if (m_impl->storageCapabilityProcess == nullptr) {
        return;
    }
    const auto schedule = m_impl->storageCapabilityPollScheduler.schedule(activityAvailable);
    if (schedule.has_value()) {
        QTimer::singleShot(schedule->delayMilliseconds, this, [this, schedule] {
            if (m_impl->storageCapabilityPollScheduler.consume(schedule->generation)) {
                processRemoteStorageCapabilityProbe();
            }
        });
    }
}

void SshSession::processRemoteStorageCapabilityProbe()
{
    if (m_impl->storageCapabilityProcess == nullptr) {
        return;
    }
    const SshCommandPollResult poll = m_impl->storageCapabilityProcess->poll();
    if (!poll.connectionLost && !poll.result.has_value()) {
        scheduleRemoteStorageCapabilityProbe(poll.activityAvailable);
        return;
    }

    if (poll.connectionLost &&
        (m_impl->session == nullptr || ssh_is_connected(m_impl->session) == 0)) {
        fail(tr("The SSH connection was lost while probing remote storage capabilities."));
        return;
    }

    const auto stage = m_impl->storageCapabilityLifecycle.stage();
    if (poll.result.has_value()) {
        if (stage == rfm::ssh::RemoteStorageCapabilityProbeStage::Posix) {
            m_impl->storageCapabilityLifecycle.recordPosixResult(*poll.result);
            m_impl->volumeCapabilityCache.update(
                rfm::ssh::RemoteLinuxVolumeService::parseCapabilities(
                    poll.result->standardOutput.toUtf8()));
        } else if (stage == rfm::ssh::RemoteStorageCapabilityProbeStage::WindowsPowerShell) {
            m_impl->storageCapabilityLifecycle.recordWindowsResult(*poll.result);
        }
    } else if (stage == rfm::ssh::RemoteStorageCapabilityProbeStage::Posix) {
        m_impl->storageCapabilityLifecycle.skipPosix();
        m_impl->volumeCapabilityCache.update({true, false, false, false, false});
    } else if (stage == rfm::ssh::RemoteStorageCapabilityProbeStage::WindowsPowerShell) {
        m_impl->storageCapabilityLifecycle.skipWindows();
    }
    m_impl->storageCapabilityProcess.reset();
    startNextRemoteStorageCapabilityProbe();
}

void SshSession::finishRemoteStorageCapabilityProbe()
{
    if (m_impl->storageCapabilityLifecycle.stage() !=
        rfm::ssh::RemoteStorageCapabilityProbeStage::Complete) {
        return;
    }
    m_impl->currentServerCapabilities.storage =
        m_impl->storageCapabilityLifecycle.capabilities();
    emit remoteStorageCapabilitiesDetected(m_impl->currentServerCapabilities.storage);
    startPendingRemoteWork();
}

void SshSession::listDirectory(quint64 requestId, QString path)
{
    if (m_impl->sftp == nullptr) {
        fail(tr("No active SFTP connection."));
        return;
    }
    if (m_impl->activeCopyJob != nullptr && m_impl->activeCopyJob->ownsInternalPath(path)) {
        emit directoryListingFailed(requestId, path,
                                    tr("This path is internal to an active remote operation."));
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
        if (name != QStringLiteral(".") && name != QStringLiteral("..") &&
            (m_impl->activeCopyJob == nullptr ||
             !m_impl->activeCopyJob->hidesListingEntry(path, name))) {
            entries.push_back({name, attributes->size, remoteModificationTime(attributes),
                               attributes->type == SSH_FILEXFER_TYPE_DIRECTORY,
                               attributes->type == SSH_FILEXFER_TYPE_SYMLINK,
                               name.startsWith(QChar{'.'})});
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

void SshSession::countDirectoryEntries(quint64 requestId, QString path)
{
    if (m_impl->sftp == nullptr) {
        emit directoryCountFailed(requestId, path);
        return;
    }
    const QByteArray encodedPath = path.toUtf8();
    sftp_dir directory = sftp_opendir(m_impl->sftp, encodedPath.constData());
    if (directory == nullptr) {
        const int directoryError = sftp_get_error(m_impl->sftp);
        emit directoryCountFailed(requestId, path);
        if (isFatalSftpError(directoryError) || m_impl->session == nullptr ||
            ssh_is_connected(m_impl->session) == 0) {
            fail(tr("The SSH connection was lost while opening %1.").arg(path));
        }
        return;
    }

    quint64 count = 0;
    while (sftp_attributes attributes = sftp_readdir(m_impl->sftp, directory)) {
        const QString name = QString::fromUtf8(attributes->name);
        if (name != QStringLiteral(".") && name != QStringLiteral("..") &&
            (m_impl->activeCopyJob == nullptr ||
             !m_impl->activeCopyJob->hidesListingEntry(path, name))) {
            ++count;
        }
        sftp_attributes_free(attributes);
    }
    const int directoryError =
        sftp_dir_eof(directory) == 0 ? sftp_get_error(m_impl->sftp) : SSH_FX_OK;
    sftp_closedir(directory);
    if (directoryError != SSH_FX_OK) {
        emit directoryCountFailed(requestId, path);
        if (isFatalSftpError(directoryError) || m_impl->session == nullptr ||
            ssh_is_connected(m_impl->session) == 0) {
            fail(tr("The SSH connection was lost while reading %1.").arg(path));
        }
        return;
    }
    emit directoryCounted(requestId, path, count);
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
    const auto& storage = m_impl->currentServerCapabilities.storage;
    if (requestId == 0 || m_impl->sftp == nullptr ||
        !rfm::ssh::RemoteStorageCapabilityProbe::linuxScannerApplicable(storage)) {
        if (requestId != 0) {
            emit storageVolumeListingUnsupported(requestId);
        }
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
    if (m_impl->currentServerCapabilities.storage.provider !=
        rfm::core::RemoteStorageProvider::Linux) {
        emit volumeOperationFinished(rfm::core::makeVolumeOperationResult(
            request, rfm::core::VolumeOperationError::ToolUnavailable,
            QStringLiteral("Remote Linux volume operations are unavailable for this session.")));
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
                                    rfm::core::SecurePassword password)
{
    const auto awaiting = m_impl->awaitingVolumeAuthentications.constFind(operationId);
    if (awaiting == m_impl->awaitingVolumeAuthentications.cend() ||
        awaiting->authenticationToken != authenticationToken || authenticationToken == 0 ||
        password.isEmpty()) {
        return;
    }
    if (m_impl->session == nullptr || m_impl->sftp == nullptr ||
        ssh_is_connected(m_impl->session) == 0) {
        const auto request = awaiting->request;
        m_impl->awaitingVolumeAuthentications.erase(awaiting);
        m_impl->activeVolumeDevices.remove(request.target.device);
        emit volumeOperationFinished(rfm::core::makeVolumeOperationResult(
            request, rfm::core::VolumeOperationError::ConnectionLost,
            QStringLiteral("The SSH connection was lost before Polkit authentication.")));
        return;
    }

    const rfm::core::VolumeOperationRequest request = awaiting->request;
    m_impl->awaitingVolumeAuthentications.erase(awaiting);
    rfm::core::VolumeOperationResult immediate;
    const bool revalidateUnmount = request.operation == rfm::core::VolumeOperation::Unmount;
    const auto command = revalidateUnmount
                             ? rfm::ssh::RemoteLinuxVolumeService::unmountTopologyCommand(
                                   request, m_impl->volumeCapabilityCache.value(), &immediate)
                             : rfm::ssh::RemoteLinuxVolumeService::interactiveOperationCommand(
                                   request, m_impl->volumeCapabilityCache.value(), &immediate);
    if (!command.has_value()) {
        m_impl->activeVolumeDevices.remove(request.target.device);
        emit volumeOperationFinished(immediate);
        return;
    }
    m_impl->volumeCommandQueue.emplace_front(SshVolumeCommandTask{
        revalidateUnmount ? SshVolumeCommandPurpose::RevalidateVolumeOperation
                          : SshVolumeCommandPurpose::InteractiveVolumeOperation,
        *command, 0, request, std::move(password)});
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
    if (m_impl->pendingStorageRequestId != 0) {
        const auto& storage = m_impl->currentServerCapabilities.storage;
        if (storage.detectionState != rfm::core::CapabilityDetectionState::Detected) {
            return;
        }
        if (storage.provider != rfm::core::RemoteStorageProvider::Linux) {
            const quint64 requestId = std::exchange(m_impl->pendingStorageRequestId, quint64{0});
            m_impl->pendingBlockDevices.clear();
            emit storageVolumeListingUnsupported(requestId);
            return;
        }
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
            m_impl->volumeCommandQueue.emplace_front(
                SshVolumeCommandTask{SshVolumeCommandPurpose::Capabilities,
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
            m_impl->volumeCommandQueue.emplace_back(SshVolumeCommandTask{
                SshVolumeCommandPurpose::BlockDevices,
                rfm::ssh::RemoteLinuxVolumeService::blockDeviceDiscoveryCommand(),
                requestId,
                {},
                {}});
        } else if (m_impl->currentServerCapabilities.storage.linuxMountInfo ==
                   rfm::core::CapabilitySupport::Supported) {
            startRemoteStorageScanner(requestId);
        } else {
            emit storageVolumeListingUnsupported(requestId);
        }
    }

    while (!m_impl->pendingVolumeOperations.isEmpty()) {
        const rfm::core::VolumeOperationRequest request = m_impl->pendingVolumeOperations.dequeue();
        rfm::core::VolumeOperationResult immediate;
        const bool revalidateUnmount = request.operation == rfm::core::VolumeOperation::Unmount;
        const std::optional<QString> command =
            revalidateUnmount ? rfm::ssh::RemoteLinuxVolumeService::unmountTopologyCommand(
                                    request, m_impl->volumeCapabilityCache.value(), &immediate)
                              : rfm::ssh::RemoteLinuxVolumeService::operationCommand(
                                    request, m_impl->volumeCapabilityCache.value(), &immediate);
        if (!command.has_value()) {
            m_impl->activeVolumeDevices.remove(request.target.device);
            emit volumeOperationFinished(immediate);
            continue;
        }
        m_impl->volumeCommandQueue.emplace_back(SshVolumeCommandTask{
            revalidateUnmount ? SshVolumeCommandPurpose::RevalidateVolumeOperation
                              : SshVolumeCommandPurpose::VolumeOperation,
            *command,
            0,
            request,
            {}});
    }
    scheduleVolumeCommandStep();
}

void SshSession::scheduleVolumeCommandStep(bool activityAvailable)
{
    if (m_impl->activeVolumeCommand == std::nullopt) {
        if (m_impl->volumeCommandQueue.empty()) {
            return;
        }
        m_impl->activeVolumeCommand = std::move(m_impl->volumeCommandQueue.front());
        m_impl->volumeCommandQueue.pop_front();
        bool started = false;
        if (m_impl->activeVolumeCommand->purpose ==
            SshVolumeCommandPurpose::InteractiveVolumeOperation) {
            m_impl->interactiveVolumeCommandProcess =
                std::make_unique<SshInteractivePolkitProcess>(m_impl->session);
            started = m_impl->interactiveVolumeCommandProcess->start(
                m_impl->activeVolumeCommand->command,
                std::move(m_impl->activeVolumeCommand->password));
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
                if (task.purpose == SshVolumeCommandPurpose::RevalidateVolumeOperation ||
                    task.purpose == SshVolumeCommandPurpose::VolumeOperation ||
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
    const auto schedule = m_impl->volumePollScheduler.schedule(activityAvailable);
    if (schedule.has_value()) {
        QTimer::singleShot(schedule->delayMilliseconds, this, [this, schedule] {
            if (m_impl->volumePollScheduler.consume(schedule->generation)) {
                processVolumeCommandStep();
            }
        });
    }
}

void SshSession::processVolumeCommandStep()
{
    if (!m_impl->activeVolumeCommand.has_value() ||
        (m_impl->volumeCommandProcess == nullptr &&
         m_impl->interactiveVolumeCommandProcess == nullptr)) {
        scheduleVolumeCommandStep();
        return;
    }
    const bool interactive =
        m_impl->activeVolumeCommand->purpose == SshVolumeCommandPurpose::InteractiveVolumeOperation;
    std::optional<rfm::core::VolumeCommandResult> commandResult;
    std::optional<rfm::core::VolumeOperationError> protocolError;
    bool connectionLost = false;
    bool activityAvailable = false;
    if (interactive) {
        const SshInteractiveCommandPollResult poll =
            m_impl->interactiveVolumeCommandProcess->poll();
        commandResult = poll.result;
        connectionLost = poll.connectionLost;
        activityAvailable = poll.activityAvailable;
        protocolError = poll.protocolError;
    } else {
        const SshCommandPollResult poll = m_impl->volumeCommandProcess->poll();
        commandResult = poll.result;
        connectionLost = poll.connectionLost;
        activityAvailable = poll.activityAvailable;
    }
    if (connectionLost) {
        const SshVolumeCommandTask& task = *m_impl->activeVolumeCommand;
        if (task.purpose == SshVolumeCommandPurpose::RevalidateVolumeOperation ||
            task.purpose == SshVolumeCommandPurpose::VolumeOperation ||
            task.purpose == SshVolumeCommandPurpose::InteractiveVolumeOperation) {
            emit volumeOperationFinished(rfm::core::makeVolumeOperationResult(
                task.operationRequest, rfm::core::VolumeOperationError::ConnectionLost,
                QStringLiteral("The SSH connection was lost during the volume operation.")));
        }
        fail(tr("The SSH connection was lost during a remote volume command."));
        return;
    }
    if (!commandResult.has_value()) {
        scheduleVolumeCommandStep(activityAvailable);
        return;
    }

    SshVolumeCommandTask task = std::move(*m_impl->activeVolumeCommand);
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
        if (m_impl->currentServerCapabilities.storage.linuxMountInfo !=
            rfm::core::CapabilitySupport::Supported) {
            QList<rfm::core::StorageVolume> volumes =
                rfm::core::mergeLinuxBlockDevices({}, m_impl->pendingBlockDevices);
            m_impl->pendingBlockDevices.clear();
            emit storageMountInfoFingerprint(task.storageRequestId,
                                             rfm::core::storageVolumeFingerprint(volumes));
            emit storageVolumesListed(task.storageRequestId, std::move(volumes));
            break;
        }
        startRemoteStorageScanner(task.storageRequestId);
        break;
    case SshVolumeCommandPurpose::RevalidateVolumeOperation: {
        rfm::core::VolumeOperationResult immediate;
        const auto prepared = rfm::ssh::RemoteLinuxVolumeService::revalidatedUnmountCommand(
            task.operationRequest, *commandResult, m_impl->volumeCapabilityCache.value(),
            task.password, &immediate);
        if (!prepared.has_value()) {
            m_impl->activeVolumeDevices.remove(task.operationRequest.target.device);
            emit volumeOperationFinished(immediate);
            break;
        }
        m_impl->volumeCommandQueue.emplace_front(SshVolumeCommandTask{
            prepared->interactive ? SshVolumeCommandPurpose::InteractiveVolumeOperation
                                  : SshVolumeCommandPurpose::VolumeOperation,
            prepared->command, 0, prepared->request,
            prepared->interactive ? std::move(task.password) : rfm::core::SecurePassword{}});
        break;
    }
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
        emit volumeOperationFinished(rfm::ssh::RemoteLinuxVolumeService::interactiveOperationResult(
            task.operationRequest, *commandResult, protocolError));
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
    const auto& storage = m_impl->currentServerCapabilities.storage;
    if (!rfm::ssh::RemoteStorageCapabilityProbe::linuxScannerApplicable(storage)) {
        emit storageMountProbeUnsupported(requestId);
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
    const QString destination = rfm::core::RemotePath::join(parent, name);
    if (m_impl->activeCopyJob != nullptr && m_impl->activeCopyJob->ownsInternalPath(destination)) {
        emit operationFinished({id,
                                rfm::core::RemoteOperationKind::CreateDirectory,
                                {{destination, destination, false,
                                  tr("This path is internal to an active remote operation.")}}});
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
    const QString destination =
        rfm::core::RemotePath::join(rfm::core::RemotePath::parent(source), newName);
    if (m_impl->activeCopyJob != nullptr &&
        (m_impl->activeCopyJob->ownsInternalPath(source) ||
         m_impl->activeCopyJob->ownsInternalPath(destination))) {
        emit operationFinished({id,
                                rfm::core::RemoteOperationKind::Rename,
                                {{source, destination, false,
                                  tr("This path is internal to an active remote operation.")}}});
        return;
    }
    SftpBackend backend(m_impl->sftp);
    rfm::core::RemoteFileOperations operations(backend);
    emit operationFinished(operations.rename(id, source, newName));
}

void SshSession::compareRemoteFilesystems(quint64 requestId, QString sourceDirectory,
                                          QString destinationDirectory)
{
    if (requestId == 0 || m_impl->sftp == nullptr || m_impl->shuttingDown ||
        m_impl->disconnecting) {
        emit remoteFilesystemsCompared(requestId, rfm::core::RemoteFilesystemRelation::Unknown);
        return;
    }
    const rfm::core::RemoteFilesystemRelation relation =
        rfm::core::remoteFilesystemRelation(remoteFileSystemId(m_impl->sftp, sourceDirectory),
                                            remoteFileSystemId(m_impl->sftp, destinationDirectory));
    emit remoteFilesystemsCompared(requestId, relation);
}

void SshSession::startRemoteOperation(rfm::core::RemoteOperationRequest request)
{
    if (m_impl->sftp == nullptr || m_impl->shuttingDown || m_impl->disconnecting) {
        emit remoteOperationExecutorFailed(tr("Invalid request or no active SFTP connection."));
        return;
    }
    if (m_impl->activeCopyJob != nullptr) {
        fail(tr("The remote operation executor received overlapping jobs."));
        return;
    }
    rfm::core::RemoteCopyMethod copyMethod = rfm::core::RemoteCopyMethod::ClientMediatedSftp;
    rfm::core::NativeServerCopyPrimitive nativePrimitive =
        rfm::core::NativeServerCopyPrimitive::None;
    if (request.kind == rfm::core::RemoteOperationKind::Copy) {
        copyMethod = rfm::core::selectRemoteCopyMethod(m_impl->currentServerCapabilities,
                                                       m_impl->remoteCopyExecutionCapabilities);
        nativePrimitive = m_impl->remoteCopyExecutionCapabilities.nativePrimitive;
        qDebug().noquote() << QStringLiteral("Remote copy method: %1")
                                  .arg(remoteCopyMethodDescription(copyMethod, nativePrimitive));
    }
    m_impl->copyBackend = std::make_unique<SshServerSideCopyBackend>(m_impl->session, m_impl->sftp,
                                                                     copyMethod, nativePrimitive);
    m_impl->activeCopyJob = std::make_unique<rfm::core::ServerSideCopyJob>(
        *m_impl->copyBackend, request.id, std::move(request.sources),
        std::move(request.destinationDirectory), request.kind);
    emit remoteOperationUpdated(m_impl->activeCopyJob->progress());
    scheduleCopyStep();
}

void SshSession::removeEntries(quint64 id, QList<rfm::core::RemoteSelection> sources,
                               bool recursive)
{
    if (m_impl->sftp == nullptr) {
        emit failed(tr("Aucune connexion SFTP active."));
        return;
    }
    if (m_impl->activeCopyJob != nullptr &&
        std::ranges::any_of(sources, [this](const rfm::core::RemoteSelection& source) {
            return m_impl->activeCopyJob->ownsInternalPath(source.path);
        })) {
        rfm::core::RemoteOperationResult rejected{id, rfm::core::RemoteOperationKind::Remove, {}};
        rejected.items.reserve(sources.size());
        for (const rfm::core::RemoteSelection& source : std::as_const(sources)) {
            rejected.items.push_back(
                {source.path,
                 {},
                 false,
                 tr("The request targets a path internal to an active remote operation.")});
        }
        emit operationFinished(rejected);
        return;
    }
    SftpBackend backend(m_impl->sftp);
    const RemoteMountInfoResult mountInfo =
        recursive ? remoteMountInfo(m_impl->sftp) : RemoteMountInfoResult{};
    const auto mountProbe = [sftp = m_impl->sftp, &mountInfo](const QString& path) {
        const QByteArray encodedPath = path.toUtf8();
        sftp_attributes attributes = sftp_lstat(sftp, encodedPath.constData());
        if (attributes == nullptr) {
            return rfm::core::RemoteMountPointState::Unknown;
        }
        const bool symbolicLink = attributes->type == SSH_FILEXFER_TYPE_SYMLINK;
        sftp_attributes_free(attributes);
        if (symbolicLink) {
            return rfm::core::RemoteMountPointState::NotMountPoint;
        }

        const std::optional<QString> canonicalPath = canonicalRemoteEntryPath(sftp, path);
        if (!canonicalPath.has_value()) {
            return rfm::core::RemoteMountPointState::Unknown;
        }
        if (mountInfo.state == RemoteMountInfoState::Available) {
            return rfm::core::linuxMountPointState(mountInfo.contents, *canonicalPath);
        }
        if (mountInfo.state == RemoteMountInfoState::Invalid) {
            return rfm::core::RemoteMountPointState::Unknown;
        }

        const std::optional<quint64> entryFileSystem = remoteFileSystemId(sftp, *canonicalPath);
        const std::optional<quint64> parentFileSystem =
            remoteFileSystemId(sftp, rfm::core::RemotePath::parent(*canonicalPath));
        if (!entryFileSystem.has_value() || !parentFileSystem.has_value()) {
            return rfm::core::RemoteMountPointState::Unknown;
        }
        return entryFileSystem == parentFileSystem ? rfm::core::RemoteMountPointState::NotMountPoint
                                                   : rfm::core::RemoteMountPointState::MountPoint;
    };
    emit operationFinished(
        rfm::ssh::detail::removeRemoteEntriesSafely(backend, id, sources, recursive, mountProbe));
}

void SshSession::startTransfer(rfm::core::TransferRequest request)
{
    m_impl->terminalTransferIds.remove(request.id);
    if (!m_impl->transfersAvailable() || m_impl->shuttingDown || m_impl->disconnecting ||
        request.id == 0 || request.source.isEmpty() || request.destination.isEmpty() ||
        request.source.contains(QChar{'\0'}) || request.destination.contains(QChar{'\0'})) {
        const QString error = tr("Invalid transfer request or no active SFTP connection.");
        emit transferExecutorFailed(error);
        publishTransferProgress({request.id,
                                 rfm::core::TransferState::Failed,
                                 request.source,
                                 request.destination,
                                 0,
                                 0,
                                 0,
                                 error,
                                 0,
                                 0,
                                 {},
                                 request.direction,
                                 request.directory});
        return;
    }
    if (m_impl->activeTransferJob != nullptr) {
        emit transferRejected(request.id, tr("Another transfer is already active."));
        return;
    }
    m_impl->transferBackend = m_impl->makeTransferBackend();
    if (m_impl->transferBackend == nullptr) {
        publishTransferProgress({request.id,
                                 rfm::core::TransferState::Failed,
                                 request.source,
                                 request.destination,
                                 0,
                                 0,
                                 0,
                                 tr("Unable to initialize the transfer backend."),
                                 0,
                                 0,
                                 {},
                                 request.direction,
                                 request.directory});
        return;
    }
    if (request.directory) {
        m_impl->activeTransferJob =
            std::make_unique<rfm::core::TransferDirectoryJob>(*m_impl->transferBackend, request);
    } else {
        m_impl->activeTransferJob =
            std::make_unique<rfm::core::TransferFileJob>(*m_impl->transferBackend, request);
    }
    scheduleTransferStep();
}

void SshSession::pauseTransfer(quint64 id)
{
    if (m_impl->activeTransferJob != nullptr && m_impl->activeTransferJob->progress().id == id &&
        m_impl->activeTransferJob->requestPause()) {
        publishTransferProgress(m_impl->activeTransferJob->progress());
        return;
    }
    emit transferRejected(id, tr("Only the active transfer can be paused."));
}

void SshSession::resumeTransfer(quint64 id)
{
    if (m_impl->activeTransferJob != nullptr && m_impl->activeTransferJob->progress().id == id &&
        m_impl->activeTransferJob->resume()) {
        publishTransferProgress(m_impl->activeTransferJob->progress());
        scheduleTransferStep();
        return;
    }
    emit transferRejected(id, tr("Only a paused active transfer can be resumed."));
}

void SshSession::cancelTransfer(quint64 id)
{
    if (m_impl->activeTransferJob != nullptr && m_impl->activeTransferJob->progress().id == id) {
        if (m_impl->activeTransferJob->requestCancel()) {
            publishTransferProgress(m_impl->activeTransferJob->progress());
            scheduleTransferStep();
        }
        return;
    }

    emit transferRejected(id, tr("Only the active transfer can be cancelled by this executor."));
}

void SshSession::cancelRemoteOperation(quint64 id)
{
    if (m_impl->activeCopyJob == nullptr || m_impl->activeCopyJob->progress().id != id ||
        !m_impl->activeCopyJob->requestCancel()) {
        return;
    }
    emit remoteOperationUpdated(m_impl->activeCopyJob->progress());
    scheduleCopyStep();
}

void SshSession::shutdownTransfers()
{
    cancelStorageProbe();
    cancelStorageScan();
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
        const bool connectionLost = !m_impl->transferBackend->connectionAlive();
        if (connectionLost && !m_impl->shuttingDown && !m_impl->disconnecting) {
            fail(tr("The SSH/SFTP connection was lost during a transfer."));
            return;
        }
        if (m_impl->activeTransferJob->isFinished()) {
            const rfm::core::TransferProgress progress = m_impl->activeTransferJob->progress();
            m_impl->activeTransferJob.reset();
            m_impl->transferBackend.reset();
            publishTransferProgress(progress);
            completeShutdownIfReady();
        } else {
            publishTransferProgress(m_impl->activeTransferJob->progress());
            scheduleTransferStep();
        }
        return;
    }
    completeShutdownIfReady();
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
        if (!rfm::ssh::transportAlive(m_impl->session, m_impl->sftp) && !m_impl->shuttingDown &&
            !m_impl->disconnecting) {
            fail(tr("The SSH/SFTP connection was lost during a remote operation."));
            return;
        }
        emit remoteOperationUpdated(m_impl->activeCopyJob->progress());
    }
    if (m_impl->activeCopyJob->isFinished()) {
        const rfm::core::RemoteOperationResult result = m_impl->activeCopyJob->result();
        m_impl->activeCopyJob.reset();
        m_impl->copyBackend.reset();
        emit remoteOperationFinished(result);
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
    m_impl->remoteCopyCapabilityProcess.reset();
    m_impl->remoteCopyCapabilityPollScheduler.cancel();
    m_impl->remoteCopyExecutionCapabilities = {};
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

void SshSession::publishTransferProgress(const rfm::core::TransferProgress& progress)
{
    if (isTerminalTransferState(progress.state)) {
        if (m_impl->terminalTransferIds.contains(progress.id)) {
            return;
        }
        m_impl->terminalTransferIds.insert(progress.id);
    }
    emit transferUpdated(progress);
}

void SshSession::terminalizeTransfer(rfm::core::TransferState state, const QString& error)
{
    std::optional<rfm::core::TransferProgress> terminalProgress;
    if (m_impl->activeTransferJob != nullptr) {
        rfm::core::TransferProgress progress = m_impl->activeTransferJob->progress();
        if (!isTerminalTransferState(progress.state)) {
            progress.state = state;
            progress.error = error;
        }
        terminalProgress = std::move(progress);
    }
    m_impl->activeTransferJob.reset();
    m_impl->transferBackend.reset();
    if (terminalProgress.has_value()) {
        publishTransferProgress(*terminalProgress);
    }
}

void SshSession::fail(const QString& message)
{
    std::optional<rfm::core::RemoteOperationResult> remoteResult;
    if (m_impl->activeTransferJob != nullptr) {
        emit transferExecutorFailed(message);
    }
    if (m_impl->activeCopyJob != nullptr) {
        m_impl->activeCopyJob->failTransport(message);
        emit remoteOperationUpdated(m_impl->activeCopyJob->progress());
        remoteResult = m_impl->activeCopyJob->result();
        emit remoteOperationExecutorFailed(message);
        emit remoteOperationFinished(*remoteResult);
        m_impl->activeCopyJob.reset();
        m_impl->copyBackend.reset();
    }
    terminalizeTransfer(rfm::core::TransferState::Failed, message);
    m_impl->reset();
    emit failed(message);
}

} // namespace rfm::ssh
