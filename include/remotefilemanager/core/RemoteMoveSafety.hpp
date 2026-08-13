#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace rfm::core
{

enum class RemoteMountPointState { NotMountPoint, MountPoint, Unknown };

struct RemoteMoveFallbackEvidence {
    std::optional<quint64> sourceContainerFileSystem;
    std::optional<quint64> destinationContainerFileSystem;
    RemoteMountPointState sourceMountPoint{RemoteMountPointState::Unknown};
};

[[nodiscard]] QString remoteMoveFileSystemContainer(const QString& entryPath);
[[nodiscard]] RemoteMountPointState linuxMountPointState(const QByteArray& mountInfo,
                                                         const QString& entryPath);
[[nodiscard]] bool allowsRemoteMoveFallback(const RemoteMoveFallbackEvidence& evidence);

} // namespace rfm::core
