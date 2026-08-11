#pragma once

#include "remotefilemanager/core/VolumeService.hpp"

#include <QByteArray>
#include <QString>

#include <optional>

namespace rfm::ssh
{

struct RemoteLinuxVolumeCapabilities {
    bool known{false};
    bool lsblk{false};
    bool udisksctl{false};
    bool mount{false};
    bool umount{false};
};

class RemoteLinuxVolumeCapabilityCache final
{
  public:
    [[nodiscard]] const RemoteLinuxVolumeCapabilities& value() const;
    void update(RemoteLinuxVolumeCapabilities capabilities);
    void reset();

  private:
    RemoteLinuxVolumeCapabilities m_capabilities;
};

class RemoteLinuxVolumeService final
{
  public:
    [[nodiscard]] static QString capabilityProbeCommand();
    [[nodiscard]] static RemoteLinuxVolumeCapabilities
    parseCapabilities(const QByteArray& standardOutput);
    [[nodiscard]] static QString blockDeviceDiscoveryCommand();
    [[nodiscard]] static std::optional<QString>
    operationCommand(const rfm::core::VolumeOperationRequest& request,
                     const RemoteLinuxVolumeCapabilities& capabilities,
                     rfm::core::VolumeOperationResult* immediateResult = nullptr);
    [[nodiscard]] static rfm::core::VolumeOperationResult
    operationResult(const rfm::core::VolumeOperationRequest& request,
                    const rfm::core::VolumeCommandResult& commandResult);
};

} // namespace rfm::ssh
