#pragma once

#include "remotefilemanager/core/ServerCapabilities.hpp"
#include "remotefilemanager/core/VolumeService.hpp"

namespace rfm::ssh
{

enum class RemoteStorageCapabilityProbeStage { Posix, WindowsPowerShell, MountInfo, Complete };

class RemoteStorageCapabilityProbe final
{
  public:
    [[nodiscard]] static QString posixCommand();
    [[nodiscard]] static QString windowsPowerShellCommand();
    [[nodiscard]] static rfm::core::CapabilitySupport
    mountInfoCapabilityFromSftpStatus(int sftpStatus, bool readSucceeded);
    [[nodiscard]] static rfm::core::RemoteStorageCapabilities
    applyPosixResult(rfm::core::RemoteStorageCapabilities capabilities,
                     const rfm::core::VolumeCommandResult& result);
    [[nodiscard]] static rfm::core::RemoteStorageCapabilities
    applyWindowsResult(rfm::core::RemoteStorageCapabilities capabilities,
                       const rfm::core::VolumeCommandResult& result);
    [[nodiscard]] static rfm::core::RemoteStorageCapabilities
    finalized(rfm::core::RemoteStorageCapabilities capabilities);
    [[nodiscard]] static bool
    linuxScannerApplicable(const rfm::core::RemoteStorageCapabilities& capabilities);
};

class RemoteStorageCapabilityLifecycle final
{
  public:
    [[nodiscard]] RemoteStorageCapabilityProbeStage stage() const;
    [[nodiscard]] const rfm::core::RemoteStorageCapabilities& capabilities() const;
    void recordPosixResult(const rfm::core::VolumeCommandResult& result);
    void skipPosix();
    void recordWindowsResult(const rfm::core::VolumeCommandResult& result);
    void skipWindows();
    void recordMountInfo(rfm::core::CapabilitySupport support);
    void reset();

  private:
    rfm::core::RemoteStorageCapabilities m_capabilities;
    RemoteStorageCapabilityProbeStage m_stage{RemoteStorageCapabilityProbeStage::Posix};
};

} // namespace rfm::ssh
