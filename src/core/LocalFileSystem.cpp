#include "remotefilemanager/core/LocalFileSystem.hpp"

#include "LocalStorageTopology.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStorageInfo>
#include <QStringList>

#include <algorithm>
#include <utility>

#ifdef Q_OS_LINUX
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#endif

namespace rfm::core
{
namespace
{

QString rootIdentity(const QString& rootPath)
{
    const QString absolutePath = QDir::cleanPath(QFileInfo(rootPath).absoluteFilePath());
    const QString canonicalPath = QFileInfo(absolutePath).canonicalFilePath();
    QString identity = canonicalPath.isEmpty() ? absolutePath : QDir::cleanPath(canonicalPath);
#ifdef Q_OS_WIN
    identity = identity.toCaseFolded();
#endif
    return identity;
}

QList<LocalStorageMount> mountedStorageSnapshot()
{
    QList<LocalStorageMount> snapshot;
    const QList<QStorageInfo> mountedVolumes = QStorageInfo::mountedVolumes();
    snapshot.reserve(mountedVolumes.size());
    for (const QStorageInfo& storage : mountedVolumes) {
        snapshot.push_back({storage.rootPath(), QFile::decodeName(storage.device()),
                            storage.fileSystemType(), storage.name(), storage.bytesTotal(),
                            storage.isReadOnly(), storage.isValid(), storage.isReady()});
    }
    return snapshot;
}

QString storageMountIdentity(const LocalStorageMount& storage)
{
    const QString rootPath = QDir::cleanPath(storage.rootPath);
    return QStringLiteral("%1\n%2\n%3\n%4")
        .arg(rootPath, storage.device, QString::fromLatin1(storage.fileSystemType),
             storage.readOnly ? QStringLiteral("ro") : QStringLiteral("rw"));
}

QByteArray storageFingerprint(const QList<LocalStorageMount>& snapshot)
{
    QStringList identities;
    identities.reserve(snapshot.size());
    for (const LocalStorageMount& storage : snapshot) {
        if (!storage.valid || !storage.ready || storage.rootPath.isEmpty()) {
            continue;
        }
        const QString rootPath = QDir::cleanPath(storage.rootPath);
        if (!rootPath.isEmpty()) {
            identities.push_back(storageMountIdentity(storage));
        }
    }
    std::ranges::sort(identities);
    return QCryptographicHash::hash(identities.join(QChar{'\n'}).toUtf8(),
                                    QCryptographicHash::Sha256);
}

} // namespace

#ifdef Q_OS_LINUX
namespace
{
detail::SysfsReadStatus sysfsErrorStatus(int error)
{
    if (error == ENOENT || error == ENOTDIR) {
        return detail::SysfsReadStatus::NotPresent;
    }
    if (error == EACCES || error == EPERM) {
        return detail::SysfsReadStatus::PermissionDenied;
    }
    return detail::SysfsReadStatus::IoError;
}

class PosixSysfsTopologyReader final : public detail::SysfsTopologyReader
{
  public:
    detail::SysfsReadStatus nodeStatus(const QString& path) override
    {
        const QByteArray encodedPath = QFile::encodeName(path);
        struct stat status{};
        if (::lstat(encodedPath.constData(), &status) == 0) {
            return detail::SysfsReadStatus::Present;
        }
        return sysfsErrorStatus(errno);
    }

    detail::SysfsTextResult readText(const QString& path, qsizetype maximumBytes) override
    {
        if (maximumBytes <= 0) {
            return {{}, detail::SysfsReadStatus::IoError};
        }
        const QByteArray encodedPath = QFile::encodeName(path);
        const int descriptor = ::open(encodedPath.constData(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) {
            return {{}, sysfsErrorStatus(errno)};
        }
        QByteArray data(maximumBytes, Qt::Uninitialized);
        const ssize_t count = ::read(descriptor, data.data(), static_cast<size_t>(maximumBytes));
        const int readError = errno;
        static_cast<void>(::close(descriptor));
        if (count < 0) {
            return {{}, sysfsErrorStatus(readError)};
        }
        data.resize(static_cast<qsizetype>(count));
        return {std::move(data), detail::SysfsReadStatus::Present};
    }

    detail::SysfsLinkResult readLink(const QString& path) override
    {
        const QByteArray encodedPath = QFile::encodeName(path);
        char target[4096];
        const ssize_t count = ::readlink(encodedPath.constData(), target, sizeof(target));
        if (count < 0) {
            return {{}, sysfsErrorStatus(errno)};
        }
        if (count == static_cast<ssize_t>(sizeof(target))) {
            return {{}, detail::SysfsReadStatus::IoError};
        }
        return {QFile::decodeName(QByteArray(target, static_cast<qsizetype>(count))),
                detail::SysfsReadStatus::Present};
    }
};

QString canonicalSysfsPath(const QString& path)
{
    const QByteArray encodedPath = QFile::encodeName(path);
    char* const resolved = ::realpath(encodedPath.constData(), nullptr);
    if (resolved == nullptr) {
        return {};
    }
    const QString result = QFile::decodeName(resolved);
    std::free(resolved);
    return QDir::cleanPath(result);
}

StorageDeviceEvidence linuxStorageDetails(const QString& device)
{
    StorageDeviceEvidence details;
    const QByteArray nativeDevice = QFile::encodeName(device);
    struct stat deviceStatus{};
    if (device.isEmpty() || ::stat(nativeDevice.constData(), &deviceStatus) != 0 ||
        !S_ISBLK(deviceStatus.st_mode)) {
        return details;
    }

    details.blockDevice = true;
    const auto majorNumber = static_cast<qulonglong>(major(deviceStatus.st_rdev));
    const auto minorNumber = static_cast<qulonglong>(minor(deviceStatus.st_rdev));
    const QString sysfsPath =
        canonicalSysfsPath(QStringLiteral("/sys/dev/block/%1:%2")
                               .arg(QString::number(majorNumber), QString::number(minorNumber)));
    if (sysfsPath.isEmpty()) {
        return details;
    }

    PosixSysfsTopologyReader reader;
    return detail::collectLinuxStorageDetails(sysfsPath, reader);
}
} // namespace
#endif

namespace detail
{

StorageDeviceEvidence collectLinuxStorageDetails(const QString& sysfsPath,
                                                 SysfsTopologyReader& reader)
{
    QList<StorageTopologyNode> ancestry;
    QSet<QString> visitedPaths;
    QString currentPath = QDir::cleanPath(sysfsPath);
    bool traversalReliable = true;
    while (currentPath.startsWith(QStringLiteral("/sys/")) && !visitedPaths.contains(currentPath)) {
        visitedPaths.insert(currentPath);
        if (reader.nodeStatus(currentPath) != SysfsReadStatus::Present) {
            traversalReliable = false;
            break;
        }

        const SysfsLinkResult subsystem =
            reader.readLink(QDir(currentPath).filePath(QStringLiteral("subsystem")));
        if (subsystem.status == SysfsReadStatus::PermissionDenied ||
            subsystem.status == SysfsReadStatus::IoError) {
            traversalReliable = false;
        }
        const QString subsystemName = subsystem.status == SysfsReadStatus::Present
                                          ? QFileInfo(subsystem.target).fileName()
                                          : QString{};
        if (subsystem.status == SysfsReadStatus::Present && subsystemName.isEmpty()) {
            traversalReliable = false;
        }

        const SysfsTextResult removable =
            reader.readText(QDir(currentPath).filePath(QStringLiteral("removable")), 8);
        bool removableValue = false;
        if (removable.status == SysfsReadStatus::Present) {
            const QByteArray value = removable.data.trimmed();
            if (value == QByteArrayLiteral("1")) {
                removableValue = true;
            } else if (value != QByteArrayLiteral("0")) {
                traversalReliable = false;
            }
        } else if (removable.status == SysfsReadStatus::PermissionDenied ||
                   removable.status == SysfsReadStatus::IoError) {
            traversalReliable = false;
        }

        SysfsTextResult model =
            reader.readText(QDir(currentPath).filePath(QStringLiteral("model")), 256);
        if (model.status != SysfsReadStatus::Present || model.data.trimmed().isEmpty()) {
            model =
                reader.readText(QDir(currentPath).filePath(QStringLiteral("device/model")), 256);
        }
        const QString modelValue = model.status == SysfsReadStatus::Present
                                       ? QString::fromUtf8(model.data).trimmed()
                                       : QString{};
        ancestry.push_back({subsystemName, removableValue, modelValue});

        const QString parentPath = QFileInfo(currentPath).dir().absolutePath();
        if (parentPath == currentPath) {
            break;
        }
        currentPath = parentPath;
    }
    const bool topologyComplete = traversalReliable && currentPath == QStringLiteral("/sys");
    return storageDeviceEvidence(ancestry, true,
                                 sysfsPath.startsWith(QStringLiteral("/sys/devices/virtual/")),
                                 topologyComplete);
}

} // namespace detail

namespace
{

StorageDeviceEvidence platformStorageDetails(const QString& device)
{
#ifdef Q_OS_LINUX
    return linuxStorageDetails(device);
#else
    Q_UNUSED(device)
    return {};
#endif
}

} // namespace

LocalDirectoryResult LocalFileSystem::listDirectory(const QString& path)
{
    const QString absolutePath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QFileInfo directoryInfo(absolutePath);
    if (!directoryInfo.exists()) {
        return {absolutePath, {}, QObject::tr("The local folder does not exist.")};
    }
    if (!directoryInfo.isDir()) {
        return {absolutePath, {}, QObject::tr("The local path is not a folder.")};
    }
    if (!directoryInfo.isReadable()) {
        return {absolutePath, {}, QObject::tr("The local folder is not readable.")};
    }

    QDir directory(absolutePath);
    const QFileInfoList infos = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    QList<RemoteEntry> entries;
    entries.reserve(infos.size());
    for (const QFileInfo& info : infos) {
        const qint64 signedSize = info.size();
        entries.push_back({info.fileName(),
                           signedSize > 0 ? static_cast<quint64>(signedSize) : quint64{0},
                           info.lastModified(), info.isDir(), info.isSymbolicLink()});
    }
    return {directory.absolutePath(), std::move(entries), {}};
}

QList<StorageVolume> LocalFileSystem::mountedVolumes() { return mountedVolumeSnapshot().volumes; }

LocalStorageSnapshot LocalFileSystem::mountedVolumeSnapshot()
{
    return makeStorageSnapshot(mountedStorageSnapshot());
}

LocalStorageSnapshot LocalFileSystem::makeStorageSnapshot(const QList<LocalStorageMount>& snapshot)
{
    QList<StorageVolume> volumes;
    QSet<QString> roots;
    const QString systemRootIdentity = rootIdentity(QDir::rootPath());
    for (const LocalStorageMount& storage : snapshot) {
        if (!storage.valid || !storage.ready || storage.rootPath.isEmpty()) {
            continue;
        }
        const QString rootPath = QDir::cleanPath(storage.rootPath);
        const QString identity = rootIdentity(rootPath);
        if (!QFileInfo(rootPath).isDir() || identity.isEmpty() || roots.contains(identity)) {
            continue;
        }
        roots.insert(identity);
        const QString device = storage.device;
        const StorageDeviceEvidence platformDetails = platformStorageDetails(device);
        const StorageClassificationEvidence evidence{
            identity == systemRootIdentity,    isNetworkFileSystem(storage.fileSystemType),
            platformDetails.blockDevice,       platformDetails.virtualBlockDevice,
            platformDetails.externalTransport, platformDetails.topologyComplete};
        const qint64 signedTotal = storage.bytesTotal;
        StorageVolume volume;
        volume.rootPath = rootPath;
        volume.device = device;
        volume.fileSystemType = storage.fileSystemType;
        volume.bytesTotal = signedTotal > 0 ? static_cast<quint64>(signedTotal) : quint64{0};
        volume.kind = classifyStorage(evidence);
        volume.removable = platformDetails.removable;
        volume.ejectable = platformDetails.ejectable;
        volume.readOnly = storage.readOnly;
        volume.fileSystemLabel = storage.fileSystemLabel.trimmed();
        volume.deviceModel = platformDetails.deviceModel;
        volume.displayName = storageDisplayName(volume.fileSystemLabel, volume.deviceModel,
                                                volume.device, volume.rootPath);
        volumes.push_back(std::move(volume));
    }
    std::ranges::sort(volumes, [](const StorageVolume& first, const StorageVolume& second) {
        return first.rootPath.compare(second.rootPath, Qt::CaseInsensitive) < 0;
    });
    return {std::move(volumes), storageFingerprint(snapshot)};
}

QByteArray LocalFileSystem::mountedVolumeFingerprint()
{
    return storageFingerprint(mountedStorageSnapshot());
}

void LocalFileSystemWorker::listDirectory(quint64 requestId, QString path)
{
    LocalDirectoryResult result = LocalFileSystem::listDirectory(path);
    if (result.succeeded()) {
        emit directoryListed(requestId, std::move(result.path), std::move(result.entries));
    } else {
        emit directoryListingFailed(requestId, std::move(result.path), std::move(result.error));
    }
}

void LocalFileSystemWorker::listVolumes()
{
    LocalStorageSnapshot snapshot = LocalFileSystem::mountedVolumeSnapshot();
    emit volumesListed(std::move(snapshot.volumes), std::move(snapshot.fingerprint));
}

void LocalFileSystemWorker::probeVolumes(quint64 requestId)
{
    emit volumesProbed(requestId, LocalFileSystem::mountedVolumeFingerprint());
}

} // namespace rfm::core
