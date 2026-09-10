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
};

[[nodiscard]] ServerCapabilities
detectedServerCapabilities(QList<SftpExtensionCapability> sftpExtensions, QDateTime detectedAt,
                           std::optional<int> sftpProtocolVersion = std::nullopt);

} // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::CapabilityDetectionState)
Q_DECLARE_METATYPE(rfm::core::CapabilitySupport)
Q_DECLARE_METATYPE(rfm::core::SftpExtensionCapability)
Q_DECLARE_METATYPE(rfm::core::ServerCapabilities)
