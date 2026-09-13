#pragma once

#include "remotefilemanager/core/RemoteMoveSafety.hpp"
#include "remotefilemanager/core/VolumeService.hpp"

#include <QHash>
#include <QStringList>

#include <optional>

namespace rfm::ssh
{

struct RemoteDeleteSftpEvidence {
    std::optional<QString> canonicalParent;
    std::optional<QString> canonicalChild;
    QString expectedChild;
    std::optional<quint64> parentFileSystem;
    std::optional<quint64> childFileSystem;
};

class RemoteDeleteSafetyProbe final
{
  public:
    [[nodiscard]] static rfm::core::RemoteMountPointState
    portableSftpMountPointState(const RemoteDeleteSftpEvidence& evidence);
    [[nodiscard]] static QString windowsCommand(const QStringList& sftpDirectories);
    [[nodiscard]] static QHash<QString, rfm::core::RemoteMountPointState>
    windowsMountPointStates(const rfm::core::VolumeCommandResult& result);
};

} // namespace rfm::ssh
