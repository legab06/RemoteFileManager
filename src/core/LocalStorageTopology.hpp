#pragma once

#include "remotefilemanager/core/Storage.hpp"

#include <QByteArray>
#include <QString>

namespace rfm::core::detail
{

enum class SysfsReadStatus { Present, NotPresent, PermissionDenied, IoError };

struct SysfsTextResult {
    QByteArray data;
    SysfsReadStatus status{SysfsReadStatus::NotPresent};
};

struct SysfsLinkResult {
    QString target;
    SysfsReadStatus status{SysfsReadStatus::NotPresent};
};

class SysfsTopologyReader
{
  public:
    virtual ~SysfsTopologyReader() = default;

    [[nodiscard]] virtual SysfsReadStatus nodeStatus(const QString& path) = 0;
    [[nodiscard]] virtual SysfsTextResult readText(const QString& path, qsizetype maximumBytes) = 0;
    [[nodiscard]] virtual SysfsLinkResult readLink(const QString& path) = 0;
};

[[nodiscard]] StorageDeviceEvidence collectLinuxStorageDetails(const QString& sysfsPath,
                                                               SysfsTopologyReader& reader);

} // namespace rfm::core::detail
