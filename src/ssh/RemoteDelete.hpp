#pragma once

#include "remotefilemanager/core/RemoteFileOperations.hpp"
#include "remotefilemanager/core/RemoteMoveSafety.hpp"
#include "remotefilemanager/core/RemotePath.hpp"

#include <QCoreApplication>

#include <functional>

namespace rfm::ssh::detail
{

using RemoteDeleteMountProbe = std::function<rfm::core::RemoteMountPointState(const QString& path)>;

[[nodiscard]] inline rfm::core::RemoteBackendResult
preflightRemoteRemovalTree(rfm::core::RemoteFileBackend& backend, const QString& path,
                           bool directory, const RemoteDeleteMountProbe& mountProbe)
{
    const rfm::core::RemoteMountPointState mountState =
        mountProbe ? mountProbe(path) : rfm::core::RemoteMountPointState::Unknown;
    if (mountState == rfm::core::RemoteMountPointState::MountPoint) {
        return {rfm::core::RemoteBackendError::Failure,
                QCoreApplication::translate(
                    "RemoteDelete",
                    "A mount point exists in the remote removal tree. Nothing was deleted.")};
    }
    if (mountState == rfm::core::RemoteMountPointState::Unknown) {
        return {
            rfm::core::RemoteBackendError::Failure,
            QCoreApplication::translate(
                "RemoteDelete", "Unable to verify remote mount boundaries. Nothing was deleted.")};
    }
    if (!directory) {
        return {};
    }

    const rfm::core::RemoteDirectoryResult listing = backend.list(path);
    if (!listing.result.succeeded()) {
        return listing.result;
    }
    for (const auto& [name, childDirectory] : listing.entries) {
        const QString childPath = rfm::core::RemotePath::join(path, name);
        if (childPath.isEmpty() || rfm::core::RemotePath::isProtected(childPath)) {
            return {
                rfm::core::RemoteBackendError::InvalidPath,
                QCoreApplication::translate("RemoteDelete", "Invalid or protected remote path.")};
        }
        const rfm::core::RemoteBackendResult childResult =
            preflightRemoteRemovalTree(backend, childPath, childDirectory, mountProbe);
        if (!childResult.succeeded()) {
            return childResult;
        }
    }
    return {};
}

[[nodiscard]] inline rfm::core::RemoteOperationResult
rejectedRemoteRemoval(quint64 id, const QList<rfm::core::RemoteSelection>& sources,
                      const QString& error)
{
    rfm::core::RemoteOperationResult operation{id, rfm::core::RemoteOperationKind::Remove, {}};
    operation.items.reserve(sources.size());
    for (const rfm::core::RemoteSelection& source : sources) {
        operation.items.push_back(
            {rfm::core::RemotePath::normalize(source.path), {}, false, error});
    }
    return operation;
}

[[nodiscard]] inline rfm::core::RemoteOperationResult
removeRemoteEntriesSafely(rfm::core::RemoteFileBackend& backend, quint64 id,
                          const QList<rfm::core::RemoteSelection>& sources, bool recursive,
                          const RemoteDeleteMountProbe& mountProbe)
{
    if (recursive) {
        for (const rfm::core::RemoteSelection& source : sources) {
            const QString path = rfm::core::RemotePath::normalize(source.path);
            if (rfm::core::RemotePath::isProtected(path)) {
                return rejectedRemoteRemoval(
                    id, sources,
                    QCoreApplication::translate("RemoteDelete",
                                                "Invalid or protected remote path."));
            }
        }
        for (const rfm::core::RemoteSelection& source : sources) {
            const QString path = rfm::core::RemotePath::normalize(source.path);
            const rfm::core::RemoteBackendResult preflight =
                preflightRemoteRemovalTree(backend, path, source.directory, mountProbe);
            if (!preflight.succeeded()) {
                const QString error =
                    preflight.detail.isEmpty()
                        ? QCoreApplication::translate("RemoteDelete",
                                                      "The remote removal preflight failed.")
                        : preflight.detail;
                return rejectedRemoteRemoval(id, sources, error);
            }
        }
    }
    rfm::core::RemoteFileOperations operations(backend);
    return operations.remove(id, sources, recursive);
}

} // namespace rfm::ssh::detail
