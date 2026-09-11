#pragma once

#include "remotefilemanager/core/RemoteCopyStrategy.hpp"
#include "remotefilemanager/core/VolumeService.hpp"

#include <QString>

namespace rfm::ssh
{

class RemoteCopyCapabilityProbe final
{
  public:
    // libssh 0.12.2 exposes copy-data discovery, but no public invocation API.
    static constexpr bool sftpCopyDataAvailable = false;

    [[nodiscard]] static QString command();
    [[nodiscard]] static rfm::core::RemoteCopyExecutionCapabilities
    capabilitiesFrom(const rfm::core::VolumeCommandResult& result);
    [[nodiscard]] static rfm::core::RemoteCopyExecutionCapabilities unavailable();
};

} // namespace rfm::ssh
