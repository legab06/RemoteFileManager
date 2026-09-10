#include "remotefilemanager/core/ServerCapabilities.hpp"

#include <algorithm>
#include <utility>

namespace rfm::core
{

ServerCapabilities detectedServerCapabilities(QList<SftpExtensionCapability> sftpExtensions,
                                              QDateTime detectedAt,
                                              std::optional<int> sftpProtocolVersion)
{
    const bool supportsCopyDataVersion1 =
        std::ranges::any_of(sftpExtensions, [](const SftpExtensionCapability& extension) {
            return extension.name == QStringLiteral("copy-data") &&
                   extension.data == QStringLiteral("1");
        });

    return {CapabilityDetectionState::Detected, std::move(detectedAt), sftpProtocolVersion,
            std::move(sftpExtensions),
            supportsCopyDataVersion1 ? CapabilitySupport::Supported
                                     : CapabilitySupport::Unsupported};
}

} // namespace rfm::core
