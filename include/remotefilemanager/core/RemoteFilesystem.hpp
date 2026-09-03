#pragma once

#include <QMetaType>
#include <QtGlobal>

#include <optional>

namespace rfm::core
{

enum class RemoteFilesystemRelation { Same, Different, Unknown };

[[nodiscard]] RemoteFilesystemRelation
remoteFilesystemRelation(std::optional<quint64> sourceFileSystem,
                         std::optional<quint64> destinationFileSystem);

} // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::RemoteFilesystemRelation)
