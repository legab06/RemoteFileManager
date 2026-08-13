#include "remotefilemanager/core/RemoteFileOperations.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QCoreApplication>

#include <algorithm>

namespace rfm::core {
namespace {

QString translated(const char* text)
{
    return QCoreApplication::translate("RemoteFileOperations", text);
}

RemoteBackendResult invalidPathResult()
{
    return {RemoteBackendError::InvalidPath, translated("Invalid or protected remote path.")};
}

RemoteBackendResult collisionResult()
{
    return {RemoteBackendError::AlreadyExists,
            translated("A remote item already exists at this destination.")};
}

bool validDestinationDirectory(const QString& path)
{
    const QString normalized = RemotePath::normalize(path);
    return !normalized.isEmpty() && normalized != QStringLiteral("..")
        && !normalized.startsWith(QStringLiteral("../"));
}

}  // namespace

bool RemoteOperationResult::allSucceeded() const
{
    return !items.isEmpty() && std::ranges::all_of(items, &RemoteItemResult::success);
}

RemoteFileOperations::RemoteFileOperations(RemoteFileBackend& backend)
    : m_backend(backend)
{
}

RemoteOperationResult RemoteFileOperations::createDirectory(
    quint64 id, const QString& parent, const QString& name)
{
    RemoteOperationResult operation{id, RemoteOperationKind::CreateDirectory, {}};
    const QString destination = RemotePath::join(parent, name);
    RemoteItemResult item{{}, destination, false, {}};
    if (destination.isEmpty()) {
        item.error = describeError(invalidPathResult());
    } else {
        const RemoteProbeResult existing = m_backend.probe(destination);
        if (existing.result.succeeded() && existing.node.exists) {
            item.error = describeError(collisionResult());
        } else if (existing.result.error != RemoteBackendError::NotFound) {
            item.error = describeError(existing.result);
        } else {
            const RemoteBackendResult result = m_backend.createDirectory(destination);
            item.success = result.succeeded();
            item.error = describeError(result);
        }
    }
    operation.items.push_back(item);
    return operation;
}

RemoteOperationResult RemoteFileOperations::rename(
    quint64 id, const QString& source, const QString& newName)
{
    RemoteOperationResult operation{id, RemoteOperationKind::Rename, {}};
    const QString normalizedSource = RemotePath::normalize(source);
    const QString destination = RemotePath::join(RemotePath::parent(normalizedSource), newName);
    RemoteItemResult item{normalizedSource, destination, false, {}};
    if (RemotePath::isProtected(normalizedSource) || destination.isEmpty()) {
        item.error = describeError(invalidPathResult());
    } else if (normalizedSource == RemotePath::normalize(destination)) {
        item.success = true;
    } else {
        const RemoteProbeResult existing = m_backend.probe(destination);
        if (existing.result.succeeded() && existing.node.exists) {
            item.error = describeError(collisionResult());
        } else if (existing.result.error != RemoteBackendError::NotFound) {
            item.error = describeError(existing.result);
        } else {
            const RemoteBackendResult result = m_backend.rename(normalizedSource, destination);
            item.success = result.succeeded();
            item.error = describeError(result);
        }
    }
    operation.items.push_back(item);
    return operation;
}

RemoteOperationResult RemoteFileOperations::move(
    quint64 id, const QList<RemoteSelection>& sources, const QString& destinationDirectory)
{
    RemoteOperationResult operation{id, RemoteOperationKind::Move, {}};
    for (const RemoteSelection& source : sources) {
        operation.items.push_back(transferOne(source, destinationDirectory, false));
    }
    return operation;
}

RemoteOperationResult RemoteFileOperations::copy(
    quint64 id, const QList<RemoteSelection>& sources, const QString& destinationDirectory)
{
    RemoteOperationResult operation{id, RemoteOperationKind::Copy, {}};
    for (const RemoteSelection& source : sources) {
        operation.items.push_back(transferOne(source, destinationDirectory, true));
    }
    return operation;
}

RemoteOperationResult RemoteFileOperations::remove(
    quint64 id, const QList<RemoteSelection>& sources, bool recursive)
{
    RemoteOperationResult operation{id, RemoteOperationKind::Remove, {}};
    for (const RemoteSelection& source : sources) {
        const QString path = RemotePath::normalize(source.path);
        RemoteItemResult item{path, {}, false, {}};
        const RemoteBackendResult result = RemotePath::isProtected(path)
            ? invalidPathResult()
            : removeTree(path, source.directory, recursive);
        item.success = result.succeeded();
        item.error = describeError(result);
        operation.items.push_back(item);
    }
    return operation;
}

RemoteItemResult RemoteFileOperations::transferOne(
    const RemoteSelection& source, const QString& destinationDirectory, bool copy)
{
    const QString normalizedSource = RemotePath::normalize(source.path);
    const QString normalizedDestinationDirectory = RemotePath::normalize(destinationDirectory);
    const QString destination = RemotePath::join(
        normalizedDestinationDirectory, RemotePath::fileName(normalizedSource));
    RemoteItemResult item{normalizedSource, destination, false, {}};
    const bool destinationInsideSource = source.directory
        && (normalizedDestinationDirectory == normalizedSource
            || normalizedDestinationDirectory.startsWith(normalizedSource + QChar{'/'}));
    if (RemotePath::isProtected(normalizedSource)
        || !validDestinationDirectory(normalizedDestinationDirectory) || destination.isEmpty()
        || normalizedSource == RemotePath::normalize(destination) || destinationInsideSource) {
        if (normalizedSource == RemotePath::normalize(destination)) {
            item.error = translated("Source and destination are identical.");
        } else if (destinationInsideSource) {
            item.error = translated("A folder cannot be copied or moved inside itself.");
        } else {
            item.error = describeError(invalidPathResult());
        }
        return item;
    }

    const RemoteProbeResult existing = m_backend.probe(destination);
    if (existing.result.succeeded() && existing.node.exists) {
        item.error = describeError(collisionResult());
        return item;
    }
    if (existing.result.error != RemoteBackendError::NotFound) {
        item.error = describeError(existing.result);
        return item;
    }

    const RemoteBackendResult result = copy
        ? m_backend.copyOnServer(normalizedSource, destination, source.directory)
        : m_backend.rename(normalizedSource, destination);
    item.success = result.succeeded();
    item.error = describeError(result);
    return item;
}

RemoteBackendResult RemoteFileOperations::removeTree(
    const QString& path, bool directory, bool recursive)
{
    if (!directory) {
        return m_backend.removeFile(path);
    }
    if (!recursive) {
        return {RemoteBackendError::Failure,
                translated("Deleting a folder requires recursive confirmation.")};
    }

    const RemoteDirectoryResult listing = m_backend.list(path);
    if (!listing.result.succeeded()) {
        return listing.result;
    }
    for (const auto& [name, childDirectory] : listing.entries) {
        const QString childPath = RemotePath::join(path, name);
        if (childPath.isEmpty() || RemotePath::isProtected(childPath)) {
            return invalidPathResult();
        }
        const RemoteBackendResult childResult = removeTree(childPath, childDirectory, true);
        if (!childResult.succeeded()) {
            return childResult;
        }
    }
    return m_backend.removeDirectory(path);
}

QString RemoteFileOperations::describeError(const RemoteBackendResult& result)
{
    if (result.succeeded()) {
        return {};
    }
    if (!result.detail.isEmpty()) {
        return result.detail;
    }
    switch (result.error) {
    case RemoteBackendError::NotFound:
        return translated("The remote item was not found.");
    case RemoteBackendError::AlreadyExists:
        return translated("A remote item already exists at this destination.");
    case RemoteBackendError::PermissionDenied:
        return translated("Permission denied by the server.");
    case RemoteBackendError::Unsupported:
        return translated("This operation is not supported by the server.");
    case RemoteBackendError::InvalidPath:
        return translated("Invalid or protected remote path.");
    case RemoteBackendError::CrossDevice:
        return translated("The source and destination are on different filesystems.");
    case RemoteBackendError::Failure:
        return translated("The remote operation failed.");
    case RemoteBackendError::None:
        return {};
    }
    return translated("The remote operation failed.");
}

}  // namespace rfm::core
