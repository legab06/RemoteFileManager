#include "remotefilemanager/ssh/RemoteCopyCapabilityProbe.hpp"

namespace rfm::ssh
{

QString RemoteCopyCapabilityProbe::command()
{
    // Fixed, read-only probe. Shells that do not implement POSIX `command` return non-zero.
    return QStringLiteral("command -v cp >/dev/null 2>&1");
}

rfm::core::RemoteCopyExecutionCapabilities
RemoteCopyCapabilityProbe::capabilitiesFrom(const rfm::core::VolumeCommandResult& result)
{
    rfm::core::RemoteCopyExecutionCapabilities capabilities;
    capabilities.sftpCopyDataAvailable = sftpCopyDataAvailable;
    if (!result.started || result.timedOut || result.crashed || result.cancelled ||
        result.exitCode < 0) {
        return capabilities;
    }
    if (result.exitCode == 0) {
        capabilities.nativeServerCopy = rfm::core::CapabilitySupport::Supported;
        capabilities.nativePrimitive = rfm::core::NativeServerCopyPrimitive::PosixCp;
    } else {
        capabilities.nativeServerCopy = rfm::core::CapabilitySupport::Unsupported;
    }
    return capabilities;
}

rfm::core::RemoteCopyExecutionCapabilities RemoteCopyCapabilityProbe::unavailable()
{
    rfm::core::RemoteCopyExecutionCapabilities capabilities;
    capabilities.sftpCopyDataAvailable = sftpCopyDataAvailable;
    return capabilities;
}

} // namespace rfm::ssh
