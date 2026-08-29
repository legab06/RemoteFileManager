#include "LocalCopyMove.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QStorageInfo>
#include <QUuid>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace rfm::core::detail
{
namespace
{

constexpr qsizetype CopyBufferSize = 256 * 1024;

QString translated(const char* text) { return QCoreApplication::translate("LocalCopyMove", text); }

Qt::CaseSensitivity localPathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

Qt::CaseSensitivity physicalPathCaseSensitivity()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // The common filesystems on these platforms are case-insensitive. A false rejection on a
    // case-sensitive volume is safer than accepting a destination that aliases the source.
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString normalizedAbsolutePath(const QString& path)
{
    if (path.trimmed().isEmpty() || path.contains(QChar{'\0'})) {
        return {};
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
}

bool pathsEqual(const QString& first, const QString& second)
{
    return first.compare(second, localPathCaseSensitivity()) == 0;
}

bool entryExists(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymbolicLink();
}

std::filesystem::path nativeFileSystemPath(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).constData());
#endif
}

QString fileSystemPathText(const std::filesystem::path& path)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.native());
#else
    const std::string native = path.native();
    return QFile::decodeName(QByteArray(native.data(), static_cast<qsizetype>(native.size())));
#endif
}

bool rawSymbolicLinkTarget(const QString& path, QString& target, QString& error)
{
    std::error_code fileSystemError;
    const std::filesystem::path rawTarget =
        std::filesystem::read_symlink(nativeFileSystemPath(path), fileSystemError);
    if (fileSystemError) {
        error = QString::fromLocal8Bit(fileSystemError.message());
        return false;
    }
    target = fileSystemPathText(rawTarget);
    return true;
}

bool createSymbolicLink(const QString& source, const QString& rawTarget, const QString& destination,
                        QString& error)
{
    std::error_code fileSystemError;
#ifdef Q_OS_WIN
    if (QFileInfo(source).isDir()) {
        std::filesystem::create_directory_symlink(
            nativeFileSystemPath(rawTarget), nativeFileSystemPath(destination), fileSystemError);
    } else {
        std::filesystem::create_symlink(nativeFileSystemPath(rawTarget),
                                        nativeFileSystemPath(destination), fileSystemError);
    }
#else
    static_cast<void>(source);
    std::filesystem::create_symlink(nativeFileSystemPath(rawTarget),
                                    nativeFileSystemPath(destination), fileSystemError);
#endif
    if (fileSystemError) {
        error = QString::fromLocal8Bit(fileSystemError.message());
        return false;
    }
    return true;
}

bool physicalPathIsAtOrBelow(const QString& path, const QString& rootPath)
{
    const QString cleanPath = QDir::cleanPath(QDir::fromNativeSeparators(path));
    QString cleanRoot = QDir::cleanPath(QDir::fromNativeSeparators(rootPath));
    const Qt::CaseSensitivity sensitivity = physicalPathCaseSensitivity();
    if (cleanPath.compare(cleanRoot, sensitivity) == 0) {
        return true;
    }
    if (!cleanRoot.endsWith(QLatin1Char('/'))) {
        cleanRoot += QLatin1Char('/');
    }
    return cleanPath.startsWith(cleanRoot, sensitivity);
}

QString physicalEntryLocation(const QString& path)
{
    const QFileInfo info(path);
    const QString canonicalParent = QFileInfo(info.absolutePath()).canonicalFilePath();
    return canonicalParent.isEmpty() ? QString{} : QDir(canonicalParent).filePath(info.fileName());
}

bool validatePhysicalDestination(const QString& source, const QString& destinationDirectory,
                                 const QString& destination, QString& error)
{
    const QString canonicalSource = QFileInfo(source).canonicalFilePath();
    const QString canonicalDestinationDirectory =
        QFileInfo(destinationDirectory).canonicalFilePath();
    if (canonicalSource.isEmpty() || canonicalDestinationDirectory.isEmpty()) {
        error =
            translated("The folder locations could not be resolved safely before the operation.");
        return false;
    }
    const QFileInfo destinationInfo(destination);
    const QString canonicalDestination =
        entryExists(destination)
            ? destinationInfo.canonicalFilePath()
            : QDir(canonicalDestinationDirectory).filePath(destinationInfo.fileName());
    if (canonicalDestination.isEmpty()) {
        error = translated(
            "The destination location could not be resolved safely before the operation.");
        return false;
    }
    if (physicalPathIsAtOrBelow(canonicalDestinationDirectory, canonicalSource) ||
        physicalPathIsAtOrBelow(canonicalDestination, canonicalSource)) {
        error = translated("A folder cannot be copied or moved inside itself.");
        return false;
    }
    return true;
}

bool cancellationRequested(const std::function<bool()>& callback) { return callback && callback(); }

QString removeEntry(const QString& path)
{
    const QFileInfo info(path);
    if (info.isSymbolicLink() || !info.isDir()) {
        if (!entryExists(path)) {
            return translated("The local entry no longer exists.");
        }
        QFile file(path);
        if (!file.remove()) {
            return file.errorString().isEmpty()
                       ? translated("The local entry could not be deleted.")
                       : file.errorString();
        }
        return {};
    }

    const QFileInfoList children = QDir(path).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& child : children) {
        const QString error = removeEntry(child.absoluteFilePath());
        if (!error.isEmpty()) {
            return error;
        }
    }
    if (!QDir(info.absolutePath()).rmdir(info.fileName())) {
        return translated("The local folder could not be deleted.");
    }
    return {};
}

bool pathsUseDifferentFileSystems(const QString& source, const QString& destination)
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
    return !sourceStorage.rootPath().isEmpty() && !destinationStorage.rootPath().isEmpty() &&
           !pathsEqual(sourceStorage.rootPath(), destinationStorage.rootPath());
}

bool validateCopiedEntry(const QString& source, const QString& destination, QString& error)
{
    const QFileInfo sourceInfo(source);
    const QFileInfo destinationInfo(destination);
    if (!entryExists(source) || !entryExists(destination)) {
        error = translated("Copy validation failed because an entry is missing.");
        return false;
    }
    if (sourceInfo.isSymbolicLink()) {
        QString sourceTarget;
        QString destinationTarget;
        QString linkError;
        if (!destinationInfo.isSymbolicLink() ||
            !rawSymbolicLinkTarget(source, sourceTarget, linkError) ||
            !rawSymbolicLinkTarget(destination, destinationTarget, linkError) ||
            sourceTarget != destinationTarget) {
            error = translated("Copy validation failed for a symbolic link.");
            return false;
        }
        return true;
    }
    if (sourceInfo.isDir()) {
        if (!destinationInfo.isDir() || destinationInfo.isSymbolicLink()) {
            error = translated("Copy validation found a different entry type.");
            return false;
        }
        const QFileInfoList sourceChildren = QDir(source).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        const QFileInfoList destinationChildren =
            QDir(destination)
                .entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden |
                               QDir::System);
        if (sourceChildren.size() != destinationChildren.size()) {
            error = translated("Copy validation found different folder contents.");
            return false;
        }
        for (const QFileInfo& child : sourceChildren) {
            if (!validateCopiedEntry(child.absoluteFilePath(),
                                     QDir(destination).filePath(child.fileName()), error)) {
                return false;
            }
        }
        return true;
    }
    if (!sourceInfo.isFile() || !destinationInfo.isFile() ||
        sourceInfo.size() != destinationInfo.size()) {
        error = translated("Copy validation found different file metadata.");
        return false;
    }
    return true;
}

class DefaultLocalFileOperationBackend final : public LocalFileOperationBackend
{
  public:
    LocalRenameResult rename(const QString& source, const QString& destination) override
    {
        if (pathsUseDifferentFileSystems(source, destination)) {
            return {LocalRenameError::CrossDevice,
                    translated("The source and destination are on different filesystems.")};
        }
        if (QDir().rename(source, destination)) {
            return {};
        }
        return {LocalRenameError::Failure, translated("The local entry could not be renamed.")};
    }

    bool validateCopy(const QString& source, const QString& destination, QString& error) override
    {
        return validateCopiedEntry(source, destination, error);
    }
};

struct EntryAttempt {
    bool success{false};
    bool cancelled{false};
    QString source;
    QString destination;
    QString error;
    QString warning;
    LocalFileOperationOutcome outcome{LocalFileOperationOutcome::Failed};
};

enum class ManifestEntryKind : char { File, Directory, SymbolicLink };

struct SourceManifestEntry {
    QString relativePath;
    QString canonicalPath;
    QByteArray contentsHash;
    QString symbolicLinkTarget;
    qint64 size{0};
    qint64 modifiedMilliseconds{0};
    QFileDevice::Permissions permissions{};
    ManifestEntryKind kind{ManifestEntryKind::File};

    bool operator==(const SourceManifestEntry&) const = default;
};

using SourceManifest = QList<SourceManifestEntry>;

qint64 modificationMilliseconds(const QFileInfo& info)
{
    return info.lastModified().isValid() ? info.lastModified().toMSecsSinceEpoch() : 0;
}

bool hashFile(QFile& file, QByteArray& hash, const std::function<bool()>& cancel, bool& cancelled,
              QString& error)
{
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    QByteArray buffer(CopyBufferSize, Qt::Uninitialized);
    while (true) {
        if (cancellationRequested(cancel)) {
            cancelled = true;
            return false;
        }
        const qint64 bytesRead = file.read(buffer.data(), buffer.size());
        if (bytesRead < 0) {
            error = file.errorString().isEmpty()
                        ? translated("The source file could not be read completely.")
                        : file.errorString();
            return false;
        }
        if (bytesRead == 0) {
            break;
        }
        hasher.addData(QByteArrayView(buffer.constData(), bytesRead));
    }
    hash = hasher.result();
    return true;
}

SourceManifestEntry manifestEntry(const QFileInfo& info, const QString& root,
                                  ManifestEntryKind kind)
{
    return {QDir(root).relativeFilePath(info.absoluteFilePath()),
            kind == ManifestEntryKind::SymbolicLink ? QString{} : info.canonicalFilePath(),
            {},
            {},
            info.size(),
            modificationMilliseconds(info),
            info.permissions(),
            kind};
}

bool captureSourceManifestEntry(const QString& path, const QString& root, SourceManifest& manifest,
                                const std::function<bool()>& cancel, bool& cancelled,
                                QString& error, bool hashContents = true)
{
    if (cancellationRequested(cancel)) {
        cancelled = true;
        return false;
    }
    const QFileInfo info(path);
    if (!entryExists(path)) {
        error = translated("The source changed while the move was being prepared.");
        return false;
    }
    if (info.isSymbolicLink()) {
        SourceManifestEntry entry = manifestEntry(info, root, ManifestEntryKind::SymbolicLink);
        if (!rawSymbolicLinkTarget(path, entry.symbolicLinkTarget, error)) {
            return false;
        }
        manifest.push_back(std::move(entry));
        return true;
    }
    if (info.isDir()) {
        manifest.push_back(manifestEntry(info, root, ManifestEntryKind::Directory));
        const QFileInfoList children = QDir(path).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo& child : children) {
            if (!captureSourceManifestEntry(child.absoluteFilePath(), root, manifest, cancel,
                                            cancelled, error, hashContents)) {
                return false;
            }
        }
        return true;
    }
    if (!info.isFile()) {
        error = translated("This local entry type is not supported for moving.");
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString().isEmpty() ? translated("The source file could not be read.")
                                             : file.errorString();
        return false;
    }
    SourceManifestEntry entry = manifestEntry(info, root, ManifestEntryKind::File);
    if (hashContents && !hashFile(file, entry.contentsHash, cancel, cancelled, error)) {
        return false;
    }
    manifest.push_back(std::move(entry));
    return true;
}

bool manifestsHaveSameMetadata(SourceManifest first, SourceManifest second)
{
    const auto lessByPath = [](const SourceManifestEntry& left, const SourceManifestEntry& right) {
        return left.relativePath < right.relativePath;
    };
    std::sort(first.begin(), first.end(), lessByPath);
    std::sort(second.begin(), second.end(), lessByPath);
    if (first.size() != second.size()) {
        return false;
    }
    for (qsizetype index = 0; index < first.size(); ++index) {
        first[index].contentsHash.clear();
        second[index].contentsHash.clear();
        if (first.at(index) != second.at(index)) {
            return false;
        }
    }
    return true;
}

bool sourceManifestMatches(const QString& source, const SourceManifest& expected,
                           const std::function<bool()>& cancel, bool& cancelled, QString& error)
{
    SourceManifest current;
    if (!captureSourceManifestEntry(source, source, current, cancel, cancelled, error)) {
        return false;
    }
    const auto lessByPath = [](const SourceManifestEntry& first,
                               const SourceManifestEntry& second) {
        return first.relativePath < second.relativePath;
    };
    SourceManifest sortedExpected = expected;
    std::sort(sortedExpected.begin(), sortedExpected.end(), lessByPath);
    std::sort(current.begin(), current.end(), lessByPath);
    if (current != sortedExpected) {
        error = translated("The source changed while the move was being prepared; it was not "
                           "removed.");
        return false;
    }
    return true;
}

EntryAttempt failure(QString source, QString destination, QString error)
{
    return {false,
            false,
            std::move(source),
            std::move(destination),
            std::move(error),
            {},
            LocalFileOperationOutcome::Failed};
}

EntryAttempt cancellation(QString source, QString destination)
{
    return {false,
            true,
            std::move(source),
            std::move(destination),
            translated("The local operation was cancelled."),
            {},
            LocalFileOperationOutcome::Cancelled};
}

QString uniqueSiblingPath(const QString& destination, const QString& purpose)
{
    const QString parent = QFileInfo(destination).absolutePath();
    const QString name = QFileInfo(destination).fileName();
    for (int attempt = 0; attempt < 8; ++attempt) {
        const QString candidate = QDir(parent).filePath(
            QStringLiteral(".%1.rfm-%2-%3.partial")
                .arg(name, purpose, QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!entryExists(candidate)) {
            return candidate;
        }
    }
    return {};
}

void preserveFileMetadata(QFile& destination, const QFileInfo& source)
{
    static_cast<void>(destination.setPermissions(source.permissions()));
    if (source.lastModified().isValid()) {
        static_cast<void>(
            destination.setFileTime(source.lastModified(), QFileDevice::FileModificationTime));
    }
}

EntryAttempt copyEntry(const QString& source, const QString& workingDestination,
                       const QString& reportedDestination, const std::function<bool()>& cancel,
                       const QString& manifestRoot = {}, SourceManifest* manifest = nullptr)
{
    if (cancellationRequested(cancel)) {
        return cancellation(source, reportedDestination);
    }
    const QFileInfo sourceInfo(source);
    if (!entryExists(source)) {
        return failure(source, reportedDestination,
                       translated("The selected local entry no longer exists."));
    }

    if (sourceInfo.isSymbolicLink()) {
        QString rawTarget;
        QString linkError;
        if (!rawSymbolicLinkTarget(source, rawTarget, linkError) ||
            !createSymbolicLink(source, rawTarget, workingDestination, linkError)) {
            return failure(source, reportedDestination,
                           translated("The symbolic link could not be copied: ") + linkError);
        }
        if (manifest != nullptr) {
            SourceManifestEntry entry =
                manifestEntry(sourceInfo, manifestRoot, ManifestEntryKind::SymbolicLink);
            entry.symbolicLinkTarget = rawTarget;
            manifest->push_back(std::move(entry));
        }
        return {
            true, false, source, reportedDestination, {}, {}, LocalFileOperationOutcome::Succeeded};
    }

    if (sourceInfo.isDir()) {
        if (!QDir().mkdir(workingDestination)) {
            return failure(source, reportedDestination,
                           translated("The destination folder could not be created."));
        }
        if (manifest != nullptr) {
            manifest->push_back(
                manifestEntry(sourceInfo, manifestRoot, ManifestEntryKind::Directory));
        }
        const QFileInfoList children = QDir(source).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo& child : children) {
            if (cancellationRequested(cancel)) {
                return cancellation(child.absoluteFilePath(),
                                    QDir(reportedDestination).filePath(child.fileName()));
            }
            EntryAttempt copied = copyEntry(child.absoluteFilePath(),
                                            QDir(workingDestination).filePath(child.fileName()),
                                            QDir(reportedDestination).filePath(child.fileName()),
                                            cancel, manifestRoot, manifest);
            if (!copied.success) {
                return copied;
            }
        }
        static_cast<void>(QFile::setPermissions(workingDestination, sourceInfo.permissions()));
        return {
            true, false, source, reportedDestination, {}, {}, LocalFileOperationOutcome::Succeeded};
    }

    if (!sourceInfo.isFile()) {
        return failure(source, reportedDestination,
                       translated("This local entry type is not supported for copying."));
    }

    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        return failure(source, reportedDestination,
                       input.errorString().isEmpty()
                           ? translated("The source file could not be read.")
                           : input.errorString());
    }
    QFile output(workingDestination);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return failure(source, reportedDestination,
                       output.errorString().isEmpty()
                           ? translated("The destination file could not be created.")
                           : output.errorString());
    }

    QCryptographicHash sourceHash(QCryptographicHash::Sha256);
    QByteArray buffer(CopyBufferSize, Qt::Uninitialized);
    while (true) {
        if (cancellationRequested(cancel)) {
            output.close();
            static_cast<void>(output.remove());
            return cancellation(source, reportedDestination);
        }
        const qint64 bytesRead = input.read(buffer.data(), buffer.size());
        if (bytesRead < 0) {
            const QString error = input.errorString().isEmpty()
                                      ? translated("The source file could not be read completely.")
                                      : input.errorString();
            output.close();
            static_cast<void>(output.remove());
            return failure(source, reportedDestination, error);
        }
        if (bytesRead == 0) {
            break;
        }
        sourceHash.addData(QByteArrayView(buffer.constData(), bytesRead));
        qint64 offset = 0;
        while (offset < bytesRead) {
            const qint64 written = output.write(buffer.constData() + offset, bytesRead - offset);
            if (written <= 0) {
                const QString error =
                    output.errorString().isEmpty()
                        ? translated("The destination file could not be written completely.")
                        : output.errorString();
                output.close();
                static_cast<void>(output.remove());
                return failure(source, reportedDestination, error);
            }
            offset += written;
        }
    }
    preserveFileMetadata(output, sourceInfo);
    output.close();
    if (manifest != nullptr) {
        SourceManifestEntry entry =
            manifestEntry(sourceInfo, manifestRoot, ManifestEntryKind::File);
        entry.contentsHash = sourceHash.result();
        manifest->push_back(std::move(entry));
    }
    return {true, false, source, reportedDestination, {}, {}, LocalFileOperationOutcome::Succeeded};
}

EntryAttempt collisionResult(const QString& source, const QString& destination,
                             LocalCollisionPolicy policy)
{
    if (policy == LocalCollisionPolicy::Skip) {
        return {false, false, source, destination, {}, {}, LocalFileOperationOutcome::Skipped};
    }
    if (policy == LocalCollisionPolicy::Cancel) {
        return cancellation(source, destination);
    }
    return {false,
            false,
            source,
            destination,
            translated("A local entry already exists at this destination."),
            {},
            LocalFileOperationOutcome::Collision};
}

EntryAttempt publishStaging(const QString& source, const QString& staging,
                            const QString& destination, LocalCollisionPolicy policy,
                            LocalFileOperationBackend& backend, const std::function<bool()>& cancel)
{
    QString backup;
    if (entryExists(destination)) {
        if (policy != LocalCollisionPolicy::Overwrite) {
            const QString cleanupError = removeEntry(staging);
            EntryAttempt collision = collisionResult(source, destination, policy);
            if (!cleanupError.isEmpty()) {
                collision.error +=
                    translated(" Temporary copy cleanup also failed: ") + cleanupError;
            }
            return collision;
        }
        backup = uniqueSiblingPath(destination, QStringLiteral("backup"));
        if (backup.isEmpty()) {
            static_cast<void>(removeEntry(staging));
            return failure(source, destination,
                           translated("A temporary backup path could not be reserved."));
        }
        const LocalRenameResult backedUp = backend.rename(destination, backup);
        if (!backedUp.succeeded()) {
            static_cast<void>(removeEntry(staging));
            return failure(source, destination,
                           translated("The existing destination could not be preserved: ") +
                               backedUp.detail);
        }
    }

    if (cancellationRequested(cancel)) {
        QString rollbackError;
        if (!backup.isEmpty()) {
            const LocalRenameResult restored = backend.rename(backup, destination);
            if (!restored.succeeded()) {
                rollbackError = translated(" The previous destination remains at ") + backup +
                                translated(" because it could not be restored: ") + restored.detail;
            }
        }
        const QString cleanupError = removeEntry(staging);
        if (!rollbackError.isEmpty()) {
            return failure(
                source, destination,
                translated("The operation was cancelled before publication.") + rollbackError +
                    (cleanupError.isEmpty()
                         ? QString{}
                         : translated(" Temporary copy cleanup also failed: ") + cleanupError));
        }
        EntryAttempt cancelled = cancellation(source, destination);
        if (!cleanupError.isEmpty()) {
            cancelled.error += translated(" Temporary copy cleanup also failed: ") + cleanupError;
        }
        return cancelled;
    }

    const LocalRenameResult promoted = backend.rename(staging, destination);
    if (!promoted.succeeded()) {
        QString rollbackError;
        if (!backup.isEmpty()) {
            const LocalRenameResult rolledBack = backend.rename(backup, destination);
            if (!rolledBack.succeeded()) {
                rollbackError = translated(" The previous destination could not be restored: ") +
                                rolledBack.detail + translated(" It remains at ") + backup +
                                QLatin1Char('.');
            }
        }
        const QString cleanupError = entryExists(staging) ? removeEntry(staging) : QString{};
        return failure(
            source, destination,
            translated("The completed copy could not be installed: ") + promoted.detail +
                rollbackError +
                (cleanupError.isEmpty()
                     ? QString{}
                     : translated(" Temporary copy cleanup also failed: ") + cleanupError));
    }

    QString warning;
    if (!backup.isEmpty()) {
        const QString cleanupError = removeEntry(backup);
        if (!cleanupError.isEmpty()) {
            warning = translated("The previous destination remains in a temporary backup: ") +
                      backup + QStringLiteral(" — ") + cleanupError;
        }
    }
    return {true, false, source, destination, {}, warning, LocalFileOperationOutcome::Succeeded};
}

EntryAttempt copyAndPublish(const QString& source, const QString& destination,
                            LocalCollisionPolicy policy, LocalFileOperationBackend& backend,
                            const std::function<bool()>& cancel,
                            SourceManifest* moveManifest = nullptr)
{
    if (entryExists(destination) && policy != LocalCollisionPolicy::Overwrite) {
        return collisionResult(source, destination, policy);
    }
    SourceManifest initialSourceMetadata;
    if (moveManifest != nullptr) {
        bool cancelled = false;
        QString metadataError;
        if (!captureSourceManifestEntry(source, source, initialSourceMetadata, cancel, cancelled,
                                        metadataError, false)) {
            return cancelled ? cancellation(source, destination)
                             : failure(source, destination, metadataError);
        }
    }
    const QString staging = uniqueSiblingPath(
        destination,
        QStringLiteral("copy-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (staging.isEmpty()) {
        return failure(source, destination,
                       translated("A temporary copy path could not be reserved."));
    }

    EntryAttempt copied = copyEntry(source, staging, destination, cancel, source, moveManifest);
    if (!copied.success) {
        if (entryExists(staging)) {
            const QString cleanupError = removeEntry(staging);
            if (!cleanupError.isEmpty()) {
                copied.error += translated(" Temporary copy cleanup also failed: ") + cleanupError;
            }
        }
        return copied;
    }
    if (moveManifest != nullptr &&
        !manifestsHaveSameMetadata(initialSourceMetadata, *moveManifest)) {
        const QString cleanupError = removeEntry(staging);
        return failure(
            source, destination,
            translated("The source changed while it was being copied; it was not "
                       "removed.") +
                (cleanupError.isEmpty()
                     ? QString{}
                     : translated(" Temporary copy cleanup also failed: ") + cleanupError));
    }
    if (cancellationRequested(cancel)) {
        const QString cleanupError = removeEntry(staging);
        EntryAttempt cancelled = cancellation(source, destination);
        if (!cleanupError.isEmpty()) {
            cancelled.error += translated(" Temporary copy cleanup also failed: ") + cleanupError;
        }
        return cancelled;
    }

    QString validationError;
    if (!backend.validateCopy(source, staging, validationError)) {
        const QString cleanupError = removeEntry(staging);
        return failure(
            source, destination,
            (validationError.isEmpty() ? translated("Copy validation failed.") : validationError) +
                (cleanupError.isEmpty()
                     ? QString{}
                     : translated(" Temporary copy cleanup also failed: ") + cleanupError));
    }
    if (cancellationRequested(cancel)) {
        const QString cleanupError = removeEntry(staging);
        EntryAttempt cancelled = cancellation(source, destination);
        if (!cleanupError.isEmpty()) {
            cancelled.error += translated(" Temporary copy cleanup also failed: ") + cleanupError;
        }
        return cancelled;
    }
    if (moveManifest != nullptr) {
        bool cancelled = false;
        QString sourceError;
        if (!sourceManifestMatches(source, *moveManifest, cancel, cancelled, sourceError)) {
            const QString cleanupError = removeEntry(staging);
            if (cancelled) {
                EntryAttempt cancelledAttempt = cancellation(source, destination);
                if (!cleanupError.isEmpty()) {
                    cancelledAttempt.error +=
                        translated(" Temporary copy cleanup also failed: ") + cleanupError;
                }
                return cancelledAttempt;
            }
            return failure(
                source, destination,
                sourceError +
                    (cleanupError.isEmpty()
                         ? QString{}
                         : translated(" Temporary copy cleanup also failed: ") + cleanupError));
        }
    }
    return publishStaging(source, staging, destination, policy, backend, cancel);
}

EntryAttempt moveEntry(const QString& source, const QString& destination,
                       LocalCollisionPolicy policy, LocalFileOperationBackend& backend,
                       const std::function<bool()>& cancel)
{
    if (cancellationRequested(cancel)) {
        return cancellation(source, destination);
    }
    const bool collision = entryExists(destination);
    if (collision && policy != LocalCollisionPolicy::Overwrite) {
        return collisionResult(source, destination, policy);
    }

    QString backup;
    if (collision) {
        backup = uniqueSiblingPath(destination, QStringLiteral("backup"));
        if (backup.isEmpty()) {
            return failure(source, destination,
                           translated("A temporary backup path could not be reserved."));
        }
        const LocalRenameResult backedUp = backend.rename(destination, backup);
        if (!backedUp.succeeded()) {
            return failure(source, destination,
                           translated("The existing destination could not be preserved: ") +
                               backedUp.detail);
        }
    }

    if (cancellationRequested(cancel)) {
        if (!backup.isEmpty()) {
            const LocalRenameResult restored = backend.rename(backup, destination);
            if (!restored.succeeded()) {
                return failure(source, destination,
                               translated("The operation was cancelled, but the previous "
                                          "destination could not be restored. It remains at ") +
                                   backup + QStringLiteral(": ") + restored.detail);
            }
        }
        return cancellation(source, destination);
    }

    const LocalRenameResult renamed = backend.rename(source, destination);
    if (renamed.succeeded()) {
        QString warning;
        if (!backup.isEmpty()) {
            const QString cleanupError = removeEntry(backup);
            if (!cleanupError.isEmpty()) {
                warning = translated("The previous destination remains in a temporary backup: ") +
                          backup + QStringLiteral(" — ") + cleanupError;
            }
        }
        return {
            true, false, source, destination, {}, warning, LocalFileOperationOutcome::Succeeded};
    }

    if (!backup.isEmpty()) {
        const LocalRenameResult restored = backend.rename(backup, destination);
        if (!restored.succeeded()) {
            return failure(
                source, destination,
                translated("The move failed and the previous destination could not be restored: ") +
                    restored.detail + translated(" It remains at ") + backup + QLatin1Char('.'));
        }
    }
    if (renamed.error != LocalRenameError::CrossDevice) {
        return failure(source, destination,
                       renamed.detail.isEmpty() ? translated("The local entry could not be moved.")
                                                : renamed.detail);
    }

    SourceManifest sourceManifest;
    EntryAttempt copied =
        copyAndPublish(source, destination, policy, backend, cancel, &sourceManifest);
    if (!copied.success) {
        return copied;
    }
    bool manifestCancelled = false;
    QString sourceValidationError;
    if (!sourceManifestMatches(source, sourceManifest, {}, manifestCancelled,
                               sourceValidationError)) {
        return failure(source, destination,
                       sourceValidationError +
                           translated(" The destination is valid and the source was preserved."));
    }
    const QString removalError = removeEntry(source);
    if (!removalError.isEmpty()) {
        return failure(
            source, destination,
            translated("The destination was validated, but the source could not be removed: ") +
                removalError);
    }
    return copied;
}

LocalFileOperationItemResult itemResult(const EntryAttempt& attempt)
{
    return {attempt.source, attempt.destination, attempt.success,
            attempt.error.isEmpty() ? attempt.warning : attempt.error, attempt.outcome};
}

} // namespace

LocalFileOperationResult executeLocalCopyMove(const LocalFileOperationRequest& request,
                                              LocalFileOperationBackend* backend,
                                              const std::function<bool()>& cancel)
{
    LocalFileOperationResult result{request.id, request.kind, {}, false};
    const QString parent = normalizedAbsolutePath(request.parentPath);
    const QString destinationDirectory = normalizedAbsolutePath(request.destinationDirectory);
    if (parent.isEmpty() || !QFileInfo(parent).isDir()) {
        result.items.push_back(itemResult(
            failure(request.parentPath, {}, translated("The current local folder is not valid."))));
        return result;
    }
    if (destinationDirectory.isEmpty() || !QFileInfo(destinationDirectory).isDir()) {
        result.items.push_back(
            itemResult(failure({}, request.destinationDirectory,
                               translated("The destination local folder is not valid."))));
        return result;
    }
    if (!QFileInfo(destinationDirectory).isWritable()) {
        result.items.push_back(
            itemResult(failure({}, destinationDirectory,
                               translated("The destination local folder is not writable."))));
        return result;
    }
    const QString physicalDestinationDirectory =
        QFileInfo(destinationDirectory).canonicalFilePath();
    if (physicalDestinationDirectory.isEmpty()) {
        result.items.push_back(itemResult(
            failure({}, destinationDirectory,
                    translated("The destination local folder could not be resolved safely."))));
        return result;
    }
    if (request.sourcePaths.isEmpty()) {
        result.items.push_back(itemResult(
            failure({}, destinationDirectory, translated("No local entry was selected."))));
        return result;
    }

    DefaultLocalFileOperationBackend defaultBackend;
    LocalFileOperationBackend& operationBackend = backend == nullptr ? defaultBackend : *backend;
    result.items.reserve(request.sourcePaths.size());
    for (qsizetype index = 0; index < request.sourcePaths.size(); ++index) {
        const QString source = normalizedAbsolutePath(request.sourcePaths.at(index));
        const bool directChild =
            !source.isEmpty() && pathsEqual(QFileInfo(source).absolutePath(), parent);
        const QString destination =
            source.isEmpty()
                ? QString{}
                : QDir(physicalDestinationDirectory).filePath(QFileInfo(source).fileName());
        const QString physicalSource = source.isEmpty() ? QString{} : physicalEntryLocation(source);
        const QString physicalDestination =
            destination.isEmpty() ? QString{} : physicalEntryLocation(destination);
        EntryAttempt attempt;
        if (cancellationRequested(cancel)) {
            attempt = cancellation(source, destination);
        } else if (!directChild) {
            attempt =
                failure(request.sourcePaths.at(index), destination,
                        translated("The selected entry is outside the current local folder."));
        } else if (!entryExists(source)) {
            attempt = failure(source, destination,
                              translated("The selected local entry no longer exists."));
        } else if (physicalSource.isEmpty() || physicalDestination.isEmpty()) {
            attempt = failure(
                source, destination,
                translated("The source or destination location could not be resolved safely."));
        } else if (physicalSource.compare(physicalDestination, physicalPathCaseSensitivity()) ==
                       0 ||
                   pathsEqual(source, destination)) {
            attempt =
                failure(source, destination, translated("Source and destination are identical."));
        } else if (!QFileInfo(source).isSymbolicLink() && QFileInfo(source).isDir()) {
            QString destinationError;
            if (!validatePhysicalDestination(source, physicalDestinationDirectory, destination,
                                             destinationError)) {
                attempt = failure(source, destination, destinationError);
            } else if (request.kind == LocalFileOperationKind::Move) {
                attempt = moveEntry(source, destination, request.collisionPolicy, operationBackend,
                                    cancel);
            } else {
                attempt = copyAndPublish(source, destination, request.collisionPolicy,
                                         operationBackend, cancel);
            }
        } else if (request.kind == LocalFileOperationKind::Move) {
            attempt =
                moveEntry(source, destination, request.collisionPolicy, operationBackend, cancel);
        } else {
            attempt = copyAndPublish(source, destination, request.collisionPolicy, operationBackend,
                                     cancel);
        }
        result.items.push_back(itemResult(attempt));
        if (attempt.cancelled) {
            result.cancelled = true;
            for (++index; index < request.sourcePaths.size(); ++index) {
                const QString remaining = normalizedAbsolutePath(request.sourcePaths.at(index));
                result.items.push_back(itemResult(
                    cancellation(remaining, remaining.isEmpty()
                                                ? QString{}
                                                : QDir(physicalDestinationDirectory)
                                                      .filePath(QFileInfo(remaining).fileName()))));
            }
            break;
        }
    }
    return result;
}

} // namespace rfm::core::detail
