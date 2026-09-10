#include "remotefilemanager/core/LocalFileSystem.hpp"

#include "LinuxMountTable.hpp"
#include "LocalCopyMove.hpp"
#include "LocalRemove.hpp"
#include "LocalStorageTopology.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutexLocker>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStringList>

#include <algorithm>
#include <filesystem>
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

QString localDeviceNumber(const QString& device)
{
#ifdef Q_OS_LINUX
    const QByteArray nativeDevice = QFile::encodeName(device);
    struct stat deviceStatus{};
    if (device.isEmpty() || ::stat(nativeDevice.constData(), &deviceStatus) != 0 ||
        !S_ISBLK(deviceStatus.st_mode)) {
        return {};
    }
    return QStringLiteral("%1:%2").arg(
        QString::number(static_cast<qulonglong>(major(deviceStatus.st_rdev))),
        QString::number(static_cast<qulonglong>(minor(deviceStatus.st_rdev))));
#else
    Q_UNUSED(device)
    return {};
#endif
}

QString localBlockIdentity(const StorageVolume& volume)
{
    const QString deviceNumber = volume.deviceNumber.trimmed();
    if (!deviceNumber.isEmpty()) {
        return QStringLiteral("number\n%1").arg(deviceNumber);
    }
    const QString device = QDir::cleanPath(volume.device.trimmed());
    if (!device.startsWith(QStringLiteral("/dev/"))) {
        return {};
    }
    const QString canonicalDevice = QFileInfo(device).canonicalFilePath();
    return QStringLiteral("device\n%1")
        .arg(canonicalDevice.isEmpty() ? device : QDir::cleanPath(canonicalDevice));
}

QList<LocalStorageMount> mountedStorageSnapshot()
{
    QList<LocalStorageMount> snapshot;
    const QList<QStorageInfo> mountedVolumes = QStorageInfo::mountedVolumes();
    snapshot.reserve(mountedVolumes.size());
    for (const QStorageInfo& storage : mountedVolumes) {
        snapshot.push_back({storage.rootPath(), QFile::decodeName(storage.device()),
                            storage.fileSystemType(), storage.name(), storage.bytesTotal(),
                            storage.isReadOnly(), storage.isValid(), storage.isReady(),
                            localDeviceNumber(QFile::decodeName(storage.device()))});
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

bool preferStorageRepresentative(const StorageVolume& candidate, const StorageVolume& current)
{
    if ((candidate.rootPath == QStringLiteral("/")) != (current.rootPath == QStringLiteral("/"))) {
        return candidate.rootPath == QStringLiteral("/");
    }
    if (candidate.rootPath.size() != current.rootPath.size()) {
        return candidate.rootPath.size() < current.rootPath.size();
    }
    return candidate.rootPath.compare(current.rootPath, Qt::CaseInsensitive) < 0;
}

QList<StorageVolume> deduplicateMountedBlockVolumes(QList<StorageVolume> volumes)
{
    QList<StorageVolume> uniqueVolumes;
    QHash<QString, qsizetype> indexesByIdentity;
    uniqueVolumes.reserve(volumes.size());
    for (StorageVolume& volume : volumes) {
        const QString identity = localBlockIdentity(volume);
        if (!volume.mounted || identity.isEmpty()) {
            uniqueVolumes.push_back(std::move(volume));
            continue;
        }
        const auto existing = indexesByIdentity.constFind(identity);
        if (existing == indexesByIdentity.cend()) {
            indexesByIdentity.insert(identity, uniqueVolumes.size());
            uniqueVolumes.push_back(std::move(volume));
        } else if (preferStorageRepresentative(volume, uniqueVolumes.at(existing.value()))) {
            uniqueVolumes[existing.value()] = std::move(volume);
        }
    }
    return uniqueVolumes;
}

QByteArray mountedStorageFingerprint(const QList<LocalStorageMount>& snapshot)
{
    QStringList identities;
    identities.reserve(snapshot.size());
    for (const LocalStorageMount& storage : snapshot) {
        if (!storage.valid || !storage.ready || storage.rootPath.isEmpty() ||
            !isStorageVolumeCandidate(storage.fileSystemType, storage.device)) {
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

QList<LocalBlockDevice> linuxBlockDeviceSnapshot()
{
#ifdef Q_OS_LINUX
    const QString program = QStandardPaths::findExecutable(QStringLiteral("lsblk"));
    if (program.isEmpty()) {
        return {};
    }
    QProcess process;
    process.setProgram(program);
    process.setArguments(
        {QStringLiteral("--json"), QStringLiteral("--bytes"), QStringLiteral("--paths"),
         QStringLiteral("--output"),
         QStringLiteral(
             "PATH,NAME,PKNAME,TYPE,FSTYPE,LABEL,SIZE,MOUNTPOINTS,RO,RM,TRAN,MODEL,MAJ:MIN")});
    process.start(QIODevice::ReadOnly);
    if (!process.waitForStarted(2'000)) {
        return {};
    }
    if (!process.waitForFinished(10'000)) {
        process.kill();
        static_cast<void>(process.waitForFinished());
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return {};
    }
    return LocalFileSystem::parseLinuxBlockDevices(process.readAllStandardOutput());
#else
    return {};
#endif
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

Qt::CaseSensitivity localPathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString normalizedAbsoluteLocalPath(const QString& path)
{
    if (path.trimmed().isEmpty() || path.contains(QChar{'\0'})) {
        return {};
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
}

bool localPathsEqual(const QString& first, const QString& second)
{
    return first.compare(second, localPathCaseSensitivity()) == 0;
}

bool localEntryExists(const QFileInfo& info) { return info.exists() || info.isSymbolicLink(); }

QString validatedParentPath(const QString& path)
{
    const QString normalized = normalizedAbsoluteLocalPath(path);
    const QFileInfo info(normalized);
    return !normalized.isEmpty() && info.exists() && info.isDir() ? normalized : QString{};
}

QString directChildPath(const QString& parentPath, const QString& sourcePath)
{
    const QString normalizedSource = normalizedAbsoluteLocalPath(sourcePath);
    if (normalizedSource.isEmpty() ||
        !localPathsEqual(QFileInfo(normalizedSource).absolutePath(), parentPath)) {
        return {};
    }
    return normalizedSource;
}

detail::RemovalBoundaryProbe platformRemovalBoundaryProbe()
{
#ifdef Q_OS_LINUX
    // Refresh before every entry, but only parse again when the mount table changes.
    return [previous = QByteArray{},
            cached = detail::RemovalBoundaryProbe{}](const QString& path) mutable {
        QFile file(QStringLiteral("/proc/self/mountinfo"));
        if (!file.open(QIODevice::ReadOnly)) {
            return detail::RemovalBoundary::Unavailable;
        }
        const QByteArray contents = file.readAll();
        if (file.error() != QFileDevice::NoError) {
            return detail::RemovalBoundary::Unavailable;
        }
        if (!cached || contents != previous) {
            cached = detail::linuxRemovalBoundaryProbe(contents);
            previous = contents;
        }
        return cached(path);
    };
#else
    return [](const QString& path) {
        const QFileInfo info(path);
        // Windows junctions can redirect traversal even within the same volume.
#ifdef Q_OS_WIN
        if (info.isJunction()) {
            return detail::RemovalBoundary::MountPoint;
        }
#endif
        const QString canonical = info.canonicalFilePath();
        const QStorageInfo storage(path);
        const QStorageInfo parent(info.absolutePath());
        if (canonical.isEmpty() || !storage.isValid() || !storage.isReady() || !parent.isValid() ||
            !parent.isReady() || storage.rootPath().isEmpty() || parent.rootPath().isEmpty()) {
            return detail::RemovalBoundary::Unavailable;
        }
        const QString root = QFileInfo(storage.rootPath()).canonicalFilePath();
        const QString parentRoot = QFileInfo(parent.rootPath()).canonicalFilePath();
        if (root.isEmpty() || parentRoot.isEmpty()) {
            return detail::RemovalBoundary::Unavailable;
        }
        // Conservative on case-sensitive macOS volumes too.
        return canonical.compare(root, Qt::CaseInsensitive) == 0 ||
                       root.compare(parentRoot, Qt::CaseInsensitive) != 0
                   ? detail::RemovalBoundary::MountPoint
                   : detail::RemovalBoundary::Clear;
    };
#endif
}

QString removeLocalEntry(const QString& path, const detail::RemovalBoundaryProbe& probe,
                         bool preflight)
{
    const QFileInfo info(path);
    if (!localEntryExists(info)) {
        return QObject::tr("The selected local entry no longer exists.");
    }
    // Never probe or enumerate a link target, including dangling symbolic links.
    if (!info.isSymbolicLink()) {
        const detail::RemovalBoundary boundary = probe(path);
        if (boundary != detail::RemovalBoundary::Clear) {
            return boundary == detail::RemovalBoundary::MountPoint
                       ? QObject::tr("Deletion refused at local mount boundary: %1").arg(path)
                       : QObject::tr("Cannot safely verify local mount boundaries: %1").arg(path);
        }
    }
    if (info.isSymbolicLink() || !info.isDir()) {
        if (preflight) {
            return {};
        }
        QFile file(path);
        if (!file.remove()) {
            return file.errorString().isEmpty()
                       ? QObject::tr("The selected local entry could not be deleted.")
                       : file.errorString();
        }
        return {};
    }

    // QDir's empty list cannot distinguish an empty directory from an enumeration error.
    // Collect immediate children with an error-reporting portable API before descending.
#ifdef Q_OS_WIN
    const std::filesystem::path nativePath(path.toStdWString());
#else
    const std::filesystem::path nativePath(QFile::encodeName(path).constData());
#endif
    std::error_code enumerationError;
    std::filesystem::directory_iterator iterator(nativePath, enumerationError);
    const std::filesystem::directory_iterator end;
    QStringList children;
    while (!enumerationError && iterator != end) {
#ifdef Q_OS_WIN
        children.push_back(QString::fromStdWString(iterator->path().native()));
#else
        children.push_back(QFile::decodeName(iterator->path().native().c_str()));
#endif
        iterator.increment(enumerationError);
    }
    if (enumerationError) {
        return QObject::tr("Cannot safely enumerate local folder %1: %2")
            .arg(path, QString::fromLocal8Bit(enumerationError.message()));
    }
    for (const QString& child : children) {
        const QString error = removeLocalEntry(child, probe, preflight);
        if (!error.isEmpty()) {
            return error;
        }
    }
    if (preflight) {
        return {};
    }
    QDir parent(info.absolutePath());
    if (!parent.rmdir(info.fileName())) {
        return QObject::tr("The local folder could not be deleted.");
    }
    return {};
}

LocalFileOperationItemResult invalidRequestResult(const QString& source, const QString& destination,
                                                  const QString& error)
{
    return {source, destination, false, error, LocalFileOperationOutcome::Failed};
}

LocalFileOperationItemResult operationItemResult(const QString& source, const QString& destination,
                                                 bool success, const QString& error)
{
    return {source, destination, success, error,
            success ? LocalFileOperationOutcome::Succeeded : LocalFileOperationOutcome::Failed};
}

} // namespace

bool LocalFileSystem::pathsUseDifferentFileSystems(const QString& source,
                                                   const QString& destination)
{
    // rename() moves directory entries, so inspect their parent filesystems. This also avoids
    // following a symbolic link target onto an unrelated volume.
    const QStorageInfo sourceStorage(QFileInfo(source).absolutePath());
    const QStorageInfo destinationStorage(QFileInfo(destination).absolutePath());
    if (!sourceStorage.isValid() || !sourceStorage.isReady() || !destinationStorage.isValid() ||
        !destinationStorage.isReady()) {
        return false;
    }
    if (!sourceStorage.device().isEmpty() && !destinationStorage.device().isEmpty()) {
        return sourceStorage.device() != destinationStorage.device();
    }
    if (sourceStorage.rootPath().isEmpty() || destinationStorage.rootPath().isEmpty()) {
        return false;
    }
    return sourceStorage.rootPath().compare(destinationStorage.rootPath(),
                                            localPathCaseSensitivity()) != 0;
}

bool LocalFileOperationResult::allSucceeded() const
{
    return !cancelled && !items.isEmpty() &&
           std::ranges::all_of(
               items, [](const LocalFileOperationItemResult& item) { return item.success; });
}

qsizetype LocalFileOperationResult::succeededCount() const
{
    return std::ranges::count_if(items, [](const LocalFileOperationItemResult& item) {
        return item.outcome == LocalFileOperationOutcome::Succeeded;
    });
}

qsizetype LocalFileOperationResult::failedCount() const
{
    return std::ranges::count_if(items, [](const LocalFileOperationItemResult& item) {
        return item.outcome == LocalFileOperationOutcome::Failed ||
               item.outcome == LocalFileOperationOutcome::Collision;
    });
}

qsizetype LocalFileOperationResult::skippedCount() const
{
    return std::ranges::count_if(items, [](const LocalFileOperationItemResult& item) {
        return item.outcome == LocalFileOperationOutcome::Skipped;
    });
}

bool LocalFileSystem::isValidName(const QString& name)
{
    if (name.trimmed().isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..") ||
        name.contains(QChar{'/'}) || name.contains(QChar{'\0'})) {
        return false;
    }
#ifdef Q_OS_WIN
    static const QString invalidCharacters = QStringLiteral("<>:\"\\|?*");
    for (const QChar character : name) {
        if (character.unicode() < 32 || invalidCharacters.contains(character)) {
            return false;
        }
    }
    if (name.endsWith(QChar{' '}) || name.endsWith(QChar{'.'})) {
        return false;
    }
    const QString baseName = name.section(QChar{'.'}, 0, 0).toUpper();
    static const QSet<QString> reservedNames{
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")};
    if (reservedNames.contains(baseName)) {
        return false;
    }
#elif defined(Q_OS_MACOS)
    if (name.contains(QChar{':'})) {
        return false;
    }
#endif
    return true;
}

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
                           info.lastModified(), info.isDir(), info.isSymbolicLink(),
                           info.isHidden()});
    }
    return {directory.absolutePath(), std::move(entries), {}};
}

LocalFileOperationResult
LocalFileSystem::executeOperation(const LocalFileOperationRequest& request,
                                  LocalFileOperationBackend* backend,
                                  const std::function<bool()>& cancellationRequested)
{
    if (request.kind == LocalFileOperationKind::Copy ||
        request.kind == LocalFileOperationKind::Move) {
        return detail::executeLocalCopyMove(request, backend, cancellationRequested);
    }
    LocalFileOperationResult result{request.id, request.kind, {}};
    const QString parentPath = validatedParentPath(request.parentPath);
    if (parentPath.isEmpty()) {
        result.items.push_back(invalidRequestResult(
            request.parentPath, {}, QObject::tr("The current local folder is not valid.")));
        return result;
    }
    if (!QFileInfo(parentPath).isWritable()) {
        result.items.push_back(invalidRequestResult(
            parentPath, {}, QObject::tr("The current local folder is not writable.")));
        return result;
    }

    if (request.kind == LocalFileOperationKind::CreateDirectory) {
        if (!isValidName(request.newName)) {
            result.items.push_back(invalidRequestResult(
                {}, {}, QObject::tr("The folder name is not valid on this platform.")));
            return result;
        }
        const QString destination = QDir(parentPath).filePath(request.newName);
        if (localEntryExists(QFileInfo(destination))) {
            result.items.push_back(invalidRequestResult(
                {}, destination, QObject::tr("A local entry with this name already exists.")));
            return result;
        }
        const bool created = QDir(parentPath).mkdir(request.newName);
        result.items.push_back(operationItemResult(
            {}, destination, created,
            created ? QString{} : QObject::tr("The local folder could not be created.")));
        return result;
    }

    if (request.kind == LocalFileOperationKind::Rename) {
        const QString source = request.sourcePaths.size() == 1
                                   ? directChildPath(parentPath, request.sourcePaths.constFirst())
                                   : QString{};
        if (source.isEmpty()) {
            result.items.push_back(invalidRequestResult(
                request.sourcePaths.value(0), {},
                QObject::tr("The selected entry is outside the current local folder.")));
            return result;
        }
        if (!localEntryExists(QFileInfo(source))) {
            result.items.push_back(invalidRequestResult(
                source, {}, QObject::tr("The selected local entry no longer exists.")));
            return result;
        }
        if (!isValidName(request.newName)) {
            result.items.push_back(invalidRequestResult(
                source, {}, QObject::tr("The new name is not valid on this platform.")));
            return result;
        }
        const QString destination = QDir(parentPath).filePath(request.newName);
        if (localEntryExists(QFileInfo(destination))) {
            result.items.push_back(invalidRequestResult(
                source, destination, QObject::tr("A local entry with this name already exists.")));
            return result;
        }
        const bool renamed = QDir(parentPath).rename(QFileInfo(source).fileName(), request.newName);
        result.items.push_back(operationItemResult(
            source, destination, renamed,
            renamed ? QString{} : QObject::tr("The local entry could not be renamed.")));
        return result;
    }

    return detail::executeLocalRemove(request);
}

LocalFileOperationResult detail::executeLocalRemove(const LocalFileOperationRequest& request,
                                                    const RemovalBoundaryProbe& probe)
{
    const RemovalBoundaryProbe activeProbe = probe ? probe : platformRemovalBoundaryProbe();
    LocalFileOperationResult result{request.id, request.kind, {}};
    const QString parentPath = validatedParentPath(request.parentPath);
    if (request.kind != LocalFileOperationKind::Remove || parentPath.isEmpty()) {
        result.items.push_back(invalidRequestResult(
            request.parentPath, {}, QObject::tr("The local deletion request is not valid.")));
        return result;
    }
    if (request.sourcePaths.isEmpty()) {
        result.items.push_back(
            invalidRequestResult({}, {}, QObject::tr("No local entry was selected for deletion.")));
        return result;
    }
    result.items.reserve(request.sourcePaths.size());
    QList<qsizetype> validSourceIndexes;
    bool preflightFailed = false;
    for (const QString& requestedPath : request.sourcePaths) {
        const QString source = directChildPath(parentPath, requestedPath);
        if (source.isEmpty()) {
            result.items.push_back(invalidRequestResult(
                requestedPath, {},
                QObject::tr("The selected entry is outside the current local folder.")));
            preflightFailed = true;
            continue;
        }
        validSourceIndexes.push_back(result.items.size());
        result.items.push_back(operationItemResult(source, {}, false, {}));
    }
    for (const qsizetype index : validSourceIndexes) {
        auto& item = result.items[index];
        item.error = removeLocalEntry(item.source, activeProbe, true);
        preflightFailed = preflightFailed || !item.error.isEmpty();
    }
    if (preflightFailed) {
        for (auto& item : result.items) {
            if (item.error.isEmpty()) {
                item.error = QObject::tr(
                    "No local entry was deleted because the selection failed safety checks.");
            }
        }
        return result;
    }
    for (auto& item : result.items) {
        const QString error = removeLocalEntry(item.source, activeProbe, false);
        item = operationItemResult(item.source, {}, error.isEmpty(), error);
    }
    return result;
}

detail::RemovalBoundaryProbe detail::linuxRemovalBoundaryProbe(const QByteArray& mountInfo)
{
    const LinuxMountTable table = parseLinuxMountTable(mountInfo);
    QSet<QString> mountPoints;
    for (const LinuxMountInfo& mount : table.mounts) {
        mountPoints.insert(mount.rootPath);
    }
    const bool reliable = table.complete && mountPoints.contains(QStringLiteral("/"));
    return [mountPoints = std::move(mountPoints), reliable](const QString& path) {
        if (!reliable) {
            return RemovalBoundary::Unavailable;
        }
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (canonical.isEmpty()) {
            return RemovalBoundary::Unavailable;
        }
        return mountPoints.contains(canonical) ? RemovalBoundary::MountPoint
                                              : RemovalBoundary::Clear;
    };
}

bool localPathIsAtOrBelow(const QString& path, const QString& rootPath)
{
    if (path.trimmed().isEmpty() || rootPath.trimmed().isEmpty()) {
        return false;
    }
    const QString normalizedPath =
        QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
    const QString normalizedRoot =
        QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(rootPath).absoluteFilePath()));
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity caseSensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity caseSensitivity = Qt::CaseSensitive;
#endif
    if (normalizedPath.compare(normalizedRoot, caseSensitivity) == 0) {
        return true;
    }
    const QString boundary =
        normalizedRoot.endsWith(QChar{'/'}) ? normalizedRoot : normalizedRoot + QChar{'/'};
    return normalizedPath.startsWith(boundary, caseSensitivity);
}

QList<StorageVolume> LocalFileSystem::mountedVolumes() { return mountedVolumeSnapshot().volumes; }

LocalStorageSnapshot LocalFileSystem::mountedVolumeSnapshot()
{
    return makeStorageSnapshot(mountedStorageSnapshot());
}

QList<StorageVolume> LocalFileSystem::storageVolumes() { return storageSnapshot().volumes; }

LocalStorageSnapshot LocalFileSystem::storageSnapshot()
{
    return makeStorageSnapshot(mountedStorageSnapshot(), linuxBlockDeviceSnapshot());
}

QList<LocalBlockDevice> LocalFileSystem::parseLinuxBlockDevices(const QByteArray& output)
{
    return rfm::core::parseLinuxBlockDevices(output);
}

LocalStorageSnapshot
LocalFileSystem::makeStorageSnapshot(const QList<LocalStorageMount>& snapshot,
                                     const QList<LocalBlockDevice>& blockDevices)
{
    QList<StorageVolume> volumes;
    QSet<QString> roots;
    const QString systemRootIdentity = rootIdentity(QDir::rootPath());
    for (const LocalStorageMount& storage : snapshot) {
        if (!storage.valid || !storage.ready || storage.rootPath.isEmpty() ||
            !isStorageVolumeCandidate(storage.fileSystemType, storage.device)) {
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
        volume.mounted = true;
        volume.deviceNumber = storage.deviceNumber.trimmed();
        volume.displayName = storageDisplayName(volume.fileSystemLabel, volume.deviceModel,
                                                volume.device, volume.rootPath);
        volumes.push_back(std::move(volume));
    }

    volumes = deduplicateMountedBlockVolumes(std::move(volumes));
    volumes = mergeLinuxBlockDevices(std::move(volumes), blockDevices);
    const QByteArray fingerprint = storageVolumeFingerprint(volumes);
    return {std::move(volumes), fingerprint};
}

QByteArray LocalFileSystem::mountedVolumeFingerprint()
{
    return mountedStorageFingerprint(mountedStorageSnapshot());
}

QByteArray LocalFileSystem::storageFingerprint() { return storageSnapshot().fingerprint; }

void LocalFileSystemWorker::listDirectory(quint64 requestId, QString path)
{
    LocalDirectoryResult result = LocalFileSystem::listDirectory(path);
    if (result.succeeded()) {
        emit directoryListed(requestId, std::move(result.path), std::move(result.entries));
    } else {
        emit directoryListingFailed(requestId, std::move(result.path), std::move(result.error));
    }
}

void LocalFileSystemWorker::countDirectoryEntries(quint64 requestId, QString path)
{
    LocalDirectoryResult result = LocalFileSystem::listDirectory(path);
    if (result.succeeded()) {
        emit directoryCounted(requestId, std::move(result.path),
                              static_cast<quint64>(result.entries.size()));
    } else {
        emit directoryCountFailed(requestId, std::move(result.path));
    }
}

void LocalFileSystemWorker::listVolumes()
{
    LocalStorageSnapshot snapshot = LocalFileSystem::storageSnapshot();
    emit volumesListed(std::move(snapshot.volumes), std::move(snapshot.fingerprint));
}

void LocalFileSystemWorker::probeVolumes(quint64 requestId)
{
    emit volumesProbed(requestId, LocalFileSystem::storageFingerprint());
}

void LocalFileOperationWorker::execute(LocalFileOperationRequest request)
{
    const quint64 operationId = request.id;
    emit started(operationId);
    emit finished(LocalFileSystem::executeOperation(
        request, nullptr, [this, operationId] {
            const QMutexLocker lock(&m_cancellationMutex);
            return m_cancelledOperationIds.contains(operationId);
        }));
    const QMutexLocker lock(&m_cancellationMutex);
    m_cancelledOperationIds.remove(operationId);
}

void LocalFileOperationWorker::requestCancellation(quint64 operationId) noexcept
{
    if (operationId != 0) {
        const QMutexLocker lock(&m_cancellationMutex);
        m_cancelledOperationIds.insert(operationId);
    }
}

} // namespace rfm::core
