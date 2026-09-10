#include "remotefilemanager/core/RemoteCopyStrategy.hpp"

namespace rfm::core
{

RemoteCopyMethod selectRemoteCopyMethod(const ServerCapabilities& capabilities)
{
    return capabilities.detectionState == CapabilityDetectionState::Detected &&
                   capabilities.copyDataVersion1 == CapabilitySupport::Supported
               ? RemoteCopyMethod::SftpCopyData
               : RemoteCopyMethod::ClientMediatedSftp;
}

} // namespace rfm::core
