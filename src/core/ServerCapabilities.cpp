#include "remotefilemanager/core/ServerCapabilities.hpp"

#include <algorithm>
#include <utility>

namespace rfm::core
{

RemoteStorageProvider selectRemoteStorageProvider(const RemoteStorageCapabilities& capabilities)
{
    // Linux wins deterministically when both families are demonstrated. This
    // preserves the existing Linux scanner and avoids selecting an OS from
    // profile metadata or a path convention.
    if (capabilities.linuxMountInfo == CapabilitySupport::Supported ||
        capabilities.lsblk == CapabilitySupport::Supported) {
        return RemoteStorageProvider::Linux;
    }
    if (capabilities.windowsPowerShell == CapabilitySupport::Supported &&
        capabilities.windowsGetVolume == CapabilitySupport::Supported) {
        return RemoteStorageProvider::WindowsPowerShell;
    }
    return RemoteStorageProvider::None;
}

bool storageDiscoverySupported(const RemoteStorageCapabilities& capabilities)
{
    return capabilities.detectionState == CapabilityDetectionState::Detected &&
           capabilities.provider != RemoteStorageProvider::None;
}

ServerCapabilities detectedServerCapabilities(QList<SftpExtensionCapability> sftpExtensions,
                                              QDateTime detectedAt,
                                              std::optional<int> sftpProtocolVersion)
{
    const bool supportsCopyDataVersion1 =
        std::ranges::any_of(sftpExtensions, [](const SftpExtensionCapability& extension) {
            return extension.name == QStringLiteral("copy-data") &&
                   extension.data == QStringLiteral("1");
        });

    return {CapabilityDetectionState::Detected,
            std::move(detectedAt),
            sftpProtocolVersion,
            std::move(sftpExtensions),
            supportsCopyDataVersion1 ? CapabilitySupport::Supported
                                     : CapabilitySupport::Unsupported,
            {}};
}

} // namespace rfm::core
