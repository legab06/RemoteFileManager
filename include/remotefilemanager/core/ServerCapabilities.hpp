#pragma once

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>

#include <optional>

namespace rfm::core
{

enum class CapabilityDetectionState { NotDetected, Detected };

enum class CapabilitySupport { Unknown, Unsupported, Supported };

// Storage discovery is intentionally modelled separately from both SFTP
// protocol extensions and remote-copy execution capabilities.  A provider is
// selected only from probes completed for the current SSH/SFTP session.
enum class RemoteStorageProvider { None, Linux, WindowsPowerShell };

struct RemoteStorageCapabilities {
    CapabilityDetectionState detectionState{CapabilityDetectionState::NotDetected};
    CapabilitySupport linuxMountInfo{CapabilitySupport::Unknown};
    CapabilitySupport lsblk{CapabilitySupport::Unknown};
    CapabilitySupport windowsPowerShell{CapabilitySupport::Unknown};
    CapabilitySupport windowsGetVolume{CapabilitySupport::Unknown};
    CapabilitySupport windowsGetDisk{CapabilitySupport::Unknown};
    RemoteStorageProvider provider{RemoteStorageProvider::None};
};

struct SftpExtensionCapability {
    QString name;
    QString data;
};

struct ServerCapabilities {
    CapabilityDetectionState detectionState{CapabilityDetectionState::NotDetected};
    QDateTime detectedAt;
    std::optional<int> sftpProtocolVersion;
    QList<SftpExtensionCapability> sftpExtensions;
    CapabilitySupport copyDataVersion1{CapabilitySupport::Unknown};
    RemoteStorageCapabilities storage;
};

[[nodiscard]] RemoteStorageProvider
selectRemoteStorageProvider(const RemoteStorageCapabilities& capabilities);

[[nodiscard]] bool storageDiscoverySupported(const RemoteStorageCapabilities& capabilities);

[[nodiscard]] ServerCapabilities
detectedServerCapabilities(QList<SftpExtensionCapability> sftpExtensions, QDateTime detectedAt,
                           std::optional<int> sftpProtocolVersion = std::nullopt);

} // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::CapabilityDetectionState)
Q_DECLARE_METATYPE(rfm::core::CapabilitySupport)
Q_DECLARE_METATYPE(rfm::core::RemoteStorageProvider)
Q_DECLARE_METATYPE(rfm::core::RemoteStorageCapabilities)
Q_DECLARE_METATYPE(rfm::core::SftpExtensionCapability)
Q_DECLARE_METATYPE(rfm::core::ServerCapabilities)
