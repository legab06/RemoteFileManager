#include "remotefilemanager/core/RemoteFilesystem.hpp"

namespace rfm::core
{

RemoteFilesystemRelation remoteFilesystemRelation(std::optional<quint64> sourceFileSystem,
                                                  std::optional<quint64> destinationFileSystem)
{
    if (!sourceFileSystem.has_value() || !destinationFileSystem.has_value()) {
        return RemoteFilesystemRelation::Unknown;
    }
    return *sourceFileSystem == *destinationFileSystem ? RemoteFilesystemRelation::Same
                                                       : RemoteFilesystemRelation::Different;
}

} // namespace rfm::core
